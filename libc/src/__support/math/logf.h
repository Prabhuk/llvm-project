//===----------------------------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Implementation selector for logf.
///
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_MATH_LOGF_H
#define LLVM_LIBC_SRC___SUPPORT_MATH_LOGF_H

#include "src/__support/macros/config.h"
#include "src/__support/macros/optimization.h"

#if defined(LIBC_MATH_HAS_SKIP_ACCURATE_PASS) &&                               \
    (defined(LIBC_MATH_HAS_SMALL_TABLES) ||                                    \
     defined(LIBC_MATH_HAS_INTERMEDIATE_COMP_IN_FLOAT))
#include "logf_float_eval.h"
#define LIBC_MATH_LOGF_IMPL float_eval
#else
#include "logf_double_eval.h"
#define LIBC_MATH_LOGF_IMPL double_eval
#endif

namespace LIBC_NAMESPACE_DECL {
namespace math {

using LIBC_MATH_LOGF_IMPL::logf;

} // namespace math
} // namespace LIBC_NAMESPACE_DECL

#undef LIBC_MATH_LOGF_IMPL

#endif // LLVM_LIBC_SRC___SUPPORT_MATH_LOGF_H
