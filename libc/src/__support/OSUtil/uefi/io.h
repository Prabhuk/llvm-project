//===---------- UEFI implementation of IO utils -----------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_OSUTIL_UEFI_IO_H
#define LLVM_LIBC_SRC___SUPPORT_OSUTIL_UEFI_IO_H

#include "hdr/types/off_t.h"
#include "hdr/types/size_t.h"
#include "hdr/types/ssize_t.h"
#include "src/__support/CPP/string_view.h"
#include "src/__support/macros/config.h"

namespace LIBC_NAMESPACE_DECL {

// The standard streams of a UEFI application are backed by the console
// protocols of the system table: stdin reads from ConIn while stdout and stderr
// write to ConOut and StdErr respectively.
//
// The streams are exposed through the same cookie based interface that the
// baremetal stdio implementation is built on, which allows UEFI to share that
// implementation. The difference is that on UEFI the cookies and the hooks are
// provided by libc itself rather than by the vendor.
//
// The cookies carry no state, they only identify the stream they stand for.
struct __llvm_libc_stdio_cookie {};

extern "C" struct __llvm_libc_stdio_cookie __llvm_libc_stdin_cookie;
extern "C" struct __llvm_libc_stdio_cookie __llvm_libc_stdout_cookie;
extern "C" struct __llvm_libc_stdio_cookie __llvm_libc_stderr_cookie;

// Return the number of bytes read, which can be less than `size` and is zero at
// end-of-file. On failure, return a negative errno value.
extern "C" ssize_t __llvm_libc_stdio_read(void *cookie, char *buf, size_t size);

// Return the number of bytes written, which can be less than `size`. On
// failure, return a negative errno value.
extern "C" ssize_t __llvm_libc_stdio_write(void *cookie, const char *buf,
                                           size_t size);

// Return the resulting absolute file position on success. On failure, return a
// negative errno value. The console streams are not seekable.
extern "C" off_t __llvm_libc_stdio_seek(void *cookie, off_t offset, int whence);

// Return 0 on success or EOF on failure, matching fclose.
extern "C" int __llvm_libc_stdio_close(void *cookie);

void write_to_stderr(cpp::string_view msg);
void write_to_stdout(cpp::string_view msg);

} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC___SUPPORT_OSUTIL_UEFI_IO_H
