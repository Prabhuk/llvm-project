//===---------- UEFI implementation of IO utils -----------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "io.h"

#include "config/app.h"
#include "hdr/errno_macros.h"
#include "hdr/stdio_macros.h"
#include "hdr/types/off_t.h"
#include "hdr/types/size_t.h"
#include "hdr/types/ssize_t.h"
#include "include/llvm-libc-types/EFI_SIMPLE_TEXT_INPUT_PROTOCOL.h"
#include "include/llvm-libc-types/EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL.h"
#include "include/llvm-libc-types/EFI_STATUS.h"
#include "include/llvm-libc-types/EFI_SYSTEM_TABLE.h"
#include "src/__support/CPP/string_view.h"
#include "src/__support/macros/config.h"
#include "src/string/memory_utils/inline_memcpy.h"

namespace LIBC_NAMESPACE_DECL {

extern "C" {
struct __llvm_libc_stdio_cookie __llvm_libc_stdin_cookie;
struct __llvm_libc_stdio_cookie __llvm_libc_stdout_cookie;
struct __llvm_libc_stdio_cookie __llvm_libc_stderr_cookie;
}

namespace {

// Number of characters handed to the console at a time.
constexpr size_t OUTPUT_CHUNK_SIZE = 128;

// Size of the line buffer used when reading from the console.
constexpr size_t INPUT_LINE_SIZE = 256;

// Characters reported by the console input protocol that get special treatment.
constexpr char16_t CHAR_END_OF_TRANSMISSION = 0x0004; // Ctrl-D
constexpr char16_t CHAR_BACKSPACE = 0x0008;
constexpr char16_t CHAR_LINE_FEED = 0x000a;
constexpr char16_t CHAR_CARRIAGE_RETURN = 0x000d;
constexpr char16_t CHAR_DELETE = 0x007f;

EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *output_console(void *cookie) {
  if (cookie == &__llvm_libc_stdout_cookie)
    return app.system_table->ConOut;
  if (cookie == &__llvm_libc_stderr_cookie) {
    // Not every firmware provides a standard error console. Fall back to the
    // regular console in that case so that the messages are not lost.
    if (app.system_table->StdErr != nullptr)
      return app.system_table->StdErr;
    return app.system_table->ConOut;
  }
  return nullptr;
}

void echo(const char16_t *str) {
  EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *console = app.system_table->ConOut;
  console->OutputString(console, str);
}

EFI_STATUS read_key(EFI_INPUT_KEY &key) {
  EFI_SIMPLE_TEXT_INPUT_PROTOCOL *console = app.system_table->ConIn;
  for (;;) {
    EFI_STATUS status = console->ReadKeyStroke(console, &key);
    if (status != EFI_NOT_READY)
      return status;
    size_t index;
    status = app.system_table->BootServices->WaitForEvent(
        1, &console->WaitForKey, &index);
    if (EFI_ERROR(status))
      return status;
  }
}

// Input is line buffered to give the application a terminal like experience:
// the characters are echoed as they are typed, backspace erases the last one
// and the line is only handed over once enter is pressed. Pressing Ctrl-D at
// the start of a line signals end-of-file.
char input_line[INPUT_LINE_SIZE];
size_t input_line_length = 0;
size_t input_line_offset = 0;

EFI_STATUS read_line() {
  input_line_length = 0;
  input_line_offset = 0;
  while (input_line_length < INPUT_LINE_SIZE) {
    EFI_INPUT_KEY key;
    EFI_STATUS status = read_key(key);
    if (EFI_ERROR(status))
      return status;

    char16_t c = key.UnicodeChar;
    if (c == CHAR_CARRIAGE_RETURN || c == CHAR_LINE_FEED) {
      input_line[input_line_length++] = '\n';
      echo(u"\r\n");
      break;
    }
    if (c == CHAR_BACKSPACE || c == CHAR_DELETE) {
      if (input_line_length > 0) {
        --input_line_length;
        echo(u"\b \b");
      }
      continue;
    }
    if (c == CHAR_END_OF_TRANSMISSION && input_line_length == 0)
      break;
    // Keys without a character (such as the arrow and function keys) as well
    // as characters outside of ASCII are ignored.
    // TODO: Encode the characters as UTF-8 instead of dropping them.
    if (c == 0 || c > 0x7f)
      continue;

    input_line[input_line_length++] = static_cast<char>(c);
    const char16_t str[2] = {c, u'\0'};
    echo(str);
  }
  return EFI_SUCCESS;
}

} // namespace

extern "C" ssize_t __llvm_libc_stdio_read(void *cookie, char *buf,
                                          size_t size) {
  if (cookie != &__llvm_libc_stdin_cookie)
    return -EBADF;
  if (size == 0)
    return 0;

  if (input_line_offset == input_line_length) {
    EFI_STATUS status = read_line();
    if (EFI_ERROR(status))
      return -EIO;
  }

  size_t available = input_line_length - input_line_offset;
  size_t count = size < available ? size : available;
  inline_memcpy(buf, input_line + input_line_offset, count);
  input_line_offset += count;
  return static_cast<ssize_t>(count);
}

extern "C" ssize_t __llvm_libc_stdio_write(void *cookie, const char *buf,
                                           size_t size) {
  EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *console = output_console(cookie);
  if (console == nullptr)
    return -EBADF;

  // The console takes null terminated UCS-2 strings, so the text is converted a
  // chunk at a time. A line feed only moves the cursor down, a carriage return
  // is therefore inserted in front of every one of them.
  // TODO: Decode UTF-8 sequences instead of widening the bytes individually.
  char16_t chunk[OUTPUT_CHUNK_SIZE + 1];
  size_t chunk_length = 0;
  size_t written = 0;
  for (size_t i = 0; i < size; ++i) {
    if (chunk_length + 2 > OUTPUT_CHUNK_SIZE) {
      chunk[chunk_length] = u'\0';
      if (EFI_ERROR(console->OutputString(console, chunk)))
        return written > 0 ? static_cast<ssize_t>(written) : -EIO;
      written = i;
      chunk_length = 0;
    }
    if (buf[i] == '\n')
      chunk[chunk_length++] = u'\r';
    chunk[chunk_length++] = static_cast<unsigned char>(buf[i]);
  }
  if (chunk_length > 0) {
    chunk[chunk_length] = u'\0';
    if (EFI_ERROR(console->OutputString(console, chunk)))
      return written > 0 ? static_cast<ssize_t>(written) : -EIO;
  }
  return static_cast<ssize_t>(size);
}

extern "C" off_t __llvm_libc_stdio_seek(void *, off_t, int) { return -ESPIPE; }

extern "C" int __llvm_libc_stdio_close(void *cookie) {
  // There is nothing to release, the consoles belong to the firmware.
  if (cookie == &__llvm_libc_stdin_cookie ||
      cookie == &__llvm_libc_stdout_cookie ||
      cookie == &__llvm_libc_stderr_cookie)
    return 0;
  return EOF;
}

void write_to_stdout(cpp::string_view msg) {
  __llvm_libc_stdio_write(&__llvm_libc_stdout_cookie, msg.data(), msg.size());
}

void write_to_stderr(cpp::string_view msg) {
  __llvm_libc_stdio_write(&__llvm_libc_stderr_cookie, msg.data(), msg.size());
}

} // namespace LIBC_NAMESPACE_DECL
