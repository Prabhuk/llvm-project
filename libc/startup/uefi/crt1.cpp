//===-- Implementation of crt for UEFI ------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "config/app.h"
#include "hdr/types/size_t.h"
#include "include/llvm-libc-types/EFI_GUID.h"
#include "include/llvm-libc-types/EFI_HANDLE.h"
#include "include/llvm-libc-types/EFI_LOADED_IMAGE_PROTOCOL.h"
#include "include/llvm-libc-types/EFI_STATUS.h"
#include "include/llvm-libc-types/EFI_SYSTEM_TABLE.h"
#include "src/__support/macros/config.h"
#include "src/__support/wchar/character_converter.h"
#include "src/__support/wchar/mbstate.h"
#include "src/stdlib/exit.h"
#include "src/stdlib/malloc.h"

extern "C" int main(int argc, char **argv, char **envp);

namespace LIBC_NAMESPACE_DECL {
namespace {

// The arguments reach the application through the load options of its image.
// When the application is started from the UEFI shell, or from a boot option
// that carries a textual argument, those hold the command line as a UCS-2
// string. The command line is converted to UTF-8 and split into arguments at
// whitespace, with double quotes grouping words that contain whitespace as
// described by the UEFI Shell specification.
struct CommandLine {
  const char16_t *text = nullptr;
  size_t length = 0;
};

CommandLine get_command_line(EFI_HANDLE image_handle) {
  EFI_GUID guid = EFI_LOADED_IMAGE_PROTOCOL_GUID;
  EFI_LOADED_IMAGE_PROTOCOL *image = nullptr;
  EFI_STATUS status = app.system_table->BootServices->HandleProtocol(
      image_handle, &guid, reinterpret_cast<void **>(&image));
  if (EFI_ERROR(status) || image->LoadOptions == nullptr)
    return {};

  // The load options are not guaranteed to be null terminated.
  CommandLine line;
  line.text = static_cast<const char16_t *>(image->LoadOptions);
  size_t limit = image->LoadOptionsSize / sizeof(char16_t);
  while (line.length < limit && line.text[line.length] != u'\0')
    ++line.length;
  return line;
}

bool is_space(char16_t c) {
  return c == u' ' || c == u'\t' || c == u'\r' || c == u'\n';
}

// Decodes the code point at `pos`, replacing unpaired surrogates with U+FFFD.
char32_t decode(const CommandLine &line, size_t &pos) {
  char32_t c = line.text[pos++];
  if (c >= 0xD800 && c <= 0xDBFF) {
    if (pos < line.length && line.text[pos] >= 0xDC00 &&
        line.text[pos] <= 0xDFFF)
      return 0x10000 + ((c - 0xD800) << 10) + (line.text[pos++] - 0xDC00);
    return 0xFFFD;
  }
  if (c >= 0xDC00 && c <= 0xDFFF)
    return 0xFFFD;
  return c;
}

// Writes the UTF-8 encoding of the code point to `out` when it is not null and
// returns the number of bytes the encoding takes.
size_t encode(char32_t code_point, char *out) {
  internal::mbstate state;
  internal::CharacterConverter converter(&state);
  converter.push(code_point);
  size_t count = 0;
  while (!converter.isEmpty()) {
    char8_t byte = converter.pop_utf8().value();
    if (out != nullptr)
      out[count] = static_cast<char>(byte);
    ++count;
  }
  return count;
}

// Splits the command line into arguments, storing the pointers in `argv` and
// the strings in `strings` when they are not null. Returns the number of
// arguments and the size of the strings, which allows a first call to size the
// allocation that a second call then fills in.
size_t split_arguments(const CommandLine &line, char **argv, char *strings,
                       size_t &strings_size) {
  size_t argc = 0;
  strings_size = 0;
  size_t pos = 0;
  while (pos < line.length) {
    while (pos < line.length && is_space(line.text[pos]))
      ++pos;
    if (pos == line.length)
      break;

    if (argv != nullptr)
      argv[argc] = strings + strings_size;
    ++argc;

    bool quoted = false;
    while (pos < line.length && (quoted || !is_space(line.text[pos]))) {
      if (line.text[pos] == u'"') {
        quoted = !quoted;
        ++pos;
        continue;
      }
      char32_t code_point = decode(line, pos);
      strings_size += encode(
          code_point, strings != nullptr ? strings + strings_size : nullptr);
    }
    if (strings != nullptr)
      strings[strings_size] = '\0';
    ++strings_size;
  }
  return argc;
}

// Returns the argument vector of the application, which is always terminated
// by a null pointer. The vector and the strings share one allocation that is
// never released.
char **make_arguments(EFI_HANDLE image_handle, int &argc) {
  static char *no_arguments[] = {nullptr};
  argc = 0;

  CommandLine line = get_command_line(image_handle);
  size_t strings_size = 0;
  size_t count = split_arguments(line, nullptr, nullptr, strings_size);
  if (count == 0)
    return no_arguments;

  char **argv =
      static_cast<char **>(malloc((count + 1) * sizeof(char *) + strings_size));
  if (argv == nullptr)
    return no_arguments;
  split_arguments(line, argv, reinterpret_cast<char *>(argv + count + 1),
                  strings_size);
  argv[count] = nullptr;
  argc = static_cast<int>(count);
  return argv;
}

} // namespace
} // namespace LIBC_NAMESPACE_DECL

extern "C" EFI_STATUS EfiMain(EFI_HANDLE image_handle,
                              EFI_SYSTEM_TABLE *system_table) {
  LIBC_NAMESPACE::app.image_handle = image_handle;
  LIBC_NAMESPACE::app.system_table = system_table;

  // There is no environment on UEFI, the applications get an empty one.
  static char *envp[] = {nullptr};

  int argc = 0;
  char **argv = LIBC_NAMESPACE::make_arguments(image_handle, argc);
  LIBC_NAMESPACE::exit(main(argc, argv, envp));
}
