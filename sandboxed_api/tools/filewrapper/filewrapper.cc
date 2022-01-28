// Copyright 2019 Google LLC
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     https://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

// Simple utility to wrap a binary file in a C++ source file.

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <utility>
#include <vector>

#include "absl/strings/ascii.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_format.h"
#include "absl/strings/str_replace.h"
#include "sandboxed_api/util/fileops.h"
#include "sandboxed_api/util/raw_logging.h"
#include "sandboxed_api/util/strerror.h"

// C-escapes a character and writes it to a file stream.
void FWriteCEscapedC(int c, FILE* out) {
  /* clang-format off */
  constexpr char kCEscapedLen[256] = {
      4, 4, 4, 4, 4, 4, 4, 4, 4, 2, 2, 4, 4, 2, 4, 4,  // \t, \n, \r
      4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4,
      1, 1, 2, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,  // "
      1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 2,  // '0'..'9'
      1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,  // 'A'..'O'
      1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 2, 1, 1, 1,  // 'P'..'Z', '\'
      1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,  // 'a'..'o'
      1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 4,  // 'p'..'z', DEL
      4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4,
      4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4,
      4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4,
      4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4,
      4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4,
      4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4,
      4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4,
      4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4,
  };
  /* clang-format on */

  int char_len = kCEscapedLen[c];
  if (char_len == 1) {
    fputc(c, out);
  } else if (char_len == 2) {
    fputc('\\', out);
    switch (c) {
      case '\0':
        fputc('0', out);
        break;
      case '\n':
        fputc('n', out);
        break;
      case '\r':
        fputc('r', out);
        break;
      case '\t':
        fputc('t', out);
        break;
      case '\"':
      case '\'':
      case '\\':
      case '?':
        fputc(c, out);
        break;
    }
  } else {
    fputc('\\', out);
    fputc('0' + c / 64, out);
    fputc('0' + (c % 64) / 8, out);
    fputc('0' + c % 8, out);
  }
}

// Small RAII class that wraps C-style FILE streams and sets up buffering.
class File {
 public:
  File(const char* name, const char* mode)
      : name_{name}, stream_{fopen(name, mode)}, buf_(4096, '\0') {
    SAPI_RAW_PCHECK(stream_ != nullptr, "Open %s", name_);
    std::setvbuf(stream_, &buf_[0], _IOFBF, buf_.size());
    Check();
  }
  ~File() { fclose(stream_); }

  void Check() {
    if (ferror(stream_)) {
      SAPI_RAW_PLOG(ERROR, "I/O on %s", name_);
      _Exit(EXIT_FAILURE);
    }
  }

  FILE* get() const { return stream_; }

 private:
  const char* name_;
  FILE* stream_;
  std::string buf_;
};

// Format literals for generating the .h file
constexpr const char kHFileHeaderFmt[] =
    R"(// Automatically generated by sapi_cc_embed_data() Bazel rule

#ifndef SANDBOXED_API_FILE_TOC_H_
#define SANDBOXED_API_FILE_TOC_H_

#include <cstddef>

struct FileToc {
  const char* name;
  const char* data;
  size_t size;
  // Not actually used/computed by sapi_cc_embed_data(), this is for
  // compatibility with legacy code.
  unsigned char md5digest[16];
};

#endif  // SANDBOXED_API_FILE_TOC_H_

#ifndef %3$s
#define %3$s

)";
constexpr const char kHNamespaceBeginFmt[] =
    R"(namespace %s {
)";
constexpr const char kHFileTocDefsFmt[] =
    R"(
const FileToc* %1$s_create();
size_t %1$s_size();
)";
constexpr const char kHNamespaceEndFmt[] =
    R"(
}  // namespace %s
)";
constexpr const char kHFileFooterFmt[] =
    R"(
#endif  // %s
)";

// Format literals for generating the .cc file out of the input files.
constexpr const char kCcFileHeaderFmt[] =
    R"(// Automatically generated by sapi_cc_embed_data() build rule

#include "%s.h"

#include "absl/base/macros.h"
#include "absl/strings/string_view.h"

)";
constexpr const char kCcNamespaceBeginFmt[] =
    R"(namespace %s {

)";
constexpr const char kCcDataBeginFmt[] =
    R"(constexpr absl::string_view %s = {")";
constexpr const char kCcDataEndFmt[] =
    R"(", %d};
)";
constexpr const char kCcFileTocDefsBegin[] =
    R"(
constexpr FileToc kToc[] = {
)";
constexpr const char kCcFileTocDefsEntryFmt[] =
    R"(    {"%1$s", %2$s.data(), %2$s.size(), {}},
)";
constexpr const char kCcFileTocDefsEndFmt[] =
    R"(
    // Terminate array
    {nullptr, nullptr, 0, {}},
};

const FileToc* %1$s_create() {
  return kToc;
}

size_t %1$s_size() {
  return ABSL_ARRAYSIZE(kToc) - 1;
}
)";
constexpr const char kCcNamespaceEndFmt[] =
    R"(
}  // namespace %s
)";

int main(int argc, char* argv[]) {
  if (argc < 7) {
    // We're not aiming for human usability here, as this tool is always run as
    // part of the build.
    absl::FPrintF(stderr,
                  "%s PACKAGE NAME NAMESPACE OUTPUT_H OUTPUT_CC INPUT...\n",
                  argv[0]);
    return EXIT_FAILURE;
  }
  char** arg = &argv[1];

  const char* package = *arg++;
  --argc;
  const char* name = *arg++;
  std::string toc_ident = absl::StrReplaceAll(name, {{"-", "_"}});
  --argc;

  const char* ns = *arg++;
  const bool have_ns = strlen(ns) > 0;
  --argc;

  {  // Write header file first.
    File out_h(*arg++, "wb");
    --argc;
    std::string header_guard = absl::StrFormat("%s_%s_H_", package, toc_ident);
    std::replace_if(
        header_guard.begin(), header_guard.end(),
        [](char c) { return !absl::ascii_isalnum(c); }, '_');
    absl::FPrintF(out_h.get(), kHFileHeaderFmt, package, toc_ident,
                  header_guard);
    if (have_ns) {
      absl::FPrintF(out_h.get(), kHNamespaceBeginFmt, ns);
    }
    absl::FPrintF(out_h.get(), kHFileTocDefsFmt, toc_ident);
    if (have_ns) {
      absl::FPrintF(out_h.get(), kHNamespaceEndFmt, ns);
    }
    absl::FPrintF(out_h.get(), kHFileFooterFmt, header_guard);
    out_h.Check();
  }

  // Write actual translation unit with the data.
  File out_cc(*arg++, "wb");
  --argc;

  std::string package_name = package;
  if (!package_name.empty()) {
    absl::StrAppend(&package_name, "/");
  }
  absl::StrAppend(&package_name, name);
  absl::FPrintF(out_cc.get(), kCcFileHeaderFmt, package_name);
  if (have_ns) {
    absl::FPrintF(out_cc.get(), kCcNamespaceBeginFmt, ns);
  }

  std::vector<std::pair<std::string, std::string>> toc_entries;
  while (argc > 1) {
    const char* in_filename = *arg++;
    --argc;
    File in(in_filename, "rb");

    std::string basename = sapi::file_util::fileops::Basename(in_filename);
    std::string ident = absl::StrCat("k", basename);
    std::replace_if(
        ident.begin(), ident.end(),
        [](char c) { return !absl::ascii_isalnum(c); }, '_');
    absl::FPrintF(out_cc.get(), kCcDataBeginFmt, ident);
    // Remember identifiers, they are needed in the kToc array.
    toc_entries.emplace_back(std::move(basename), std::move(ident));

    int c;
    while ((c = fgetc(in.get())) != EOF) {
      FWriteCEscapedC(c, out_cc.get());
    }
    in.Check();

    absl::FPrintF(out_cc.get(), kCcDataEndFmt, ftell(in.get()));
  }
  absl::FPrintF(out_cc.get(), kCcFileTocDefsBegin);
  for (const auto& entry : toc_entries) {
    absl::FPrintF(out_cc.get(), kCcFileTocDefsEntryFmt, entry.first,
                  entry.second);
  }
  absl::FPrintF(out_cc.get(), kCcFileTocDefsEndFmt, toc_ident);

  if (have_ns) {
    absl::FPrintF(out_cc.get(), kCcNamespaceEndFmt, ns);
  }

  out_cc.Check();
  return EXIT_SUCCESS;
}
