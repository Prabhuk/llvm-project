//===----------------------------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Float-only implementation of log10f.
///
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_MATH_LOG10F_FLOAT_EVAL_H
#define LLVM_LIBC_SRC___SUPPORT_MATH_LOG10F_FLOAT_EVAL_H

#include "log2f_float_utils.h"
#include "src/__support/FPUtil/FEnvImpl.h"
#include "src/__support/FPUtil/FPBits.h"
#include "src/__support/FPUtil/double_double.h"
#include "src/__support/FPUtil/multiply_add.h"
#include "src/__support/common.h"
#include "src/__support/macros/config.h"
#include "src/__support/macros/optimization.h" // LIBC_UNLIKELY

namespace LIBC_NAMESPACE_DECL {
namespace math {
namespace float_eval {

// Float-only log10f:
//   log10f(x) = log2(x) * LOG10_2,
// where log2(x) is evaluated in FloatFloat precision with relative error
// < 2^-33 by log2f_float_utils.h (shared with powf and log2f), and the product
// with LOG10_2 is computed in FloatFloat before the final rounding.
LIBC_INLINE float log10f(float x) {
  using FPBits = fputil::FPBits<float>;
  using namespace log2f_float_internal;

  FPBits xbits(x);
  uint32_t x_u = xbits.uintval();
  int e_x = -FPBits::EXP_BIAS;

  // Exact powers of 10, whose results must be exact in all rounding modes.
  if (LIBC_UNLIKELY((x_u & 0x3FF) == 0)) {
    switch (x_u) {
    case 0x3f80'0000U: // x = 1
      return 0.0f;
    case 0x4120'0000U: // x = 10
      return 1.0f;
    case 0x42c8'0000U: // x = 100
      return 2.0f;
    case 0x447a'0000U: // x = 1,000
      return 3.0f;
    case 0x461c'4000U: // x = 10,000
      return 4.0f;
    case 0x47c3'5000U: // x = 100,000
      return 5.0f;
    case 0x4974'2400U: // x = 1,000,000
      return 6.0f;
    }
  } else {
    switch (x_u) {
    case 0x4b18'9680U: // x = 10,000,000
      return 7.0f;
    case 0x4cbe'bc20U: // x = 100,000,000
      return 8.0f;
    case 0x4e6e'6b28U: // x = 1,000,000,000
      return 9.0f;
    case 0x5015'02f9U: // x = 10,000,000,000
      return 10.0f;
    }
  }

  // Exceptional inputs.
  if (LIBC_UNLIKELY(x_u < FPBits::min_normal().uintval() ||
                    x_u > FPBits::max_normal().uintval())) {
    if (x == 0.0f) {
      fputil::set_errno_if_required(ERANGE);
      fputil::raise_except_if_required(FE_DIVBYZERO);
      return FPBits::inf(Sign::NEG).get_val();
    }
    if (xbits.is_neg() && !xbits.is_nan()) {
      fputil::set_errno_if_required(EDOM);
      fputil::raise_except_if_required(FE_INVALID);
      return FPBits::quiet_nan().get_val();
    }
    if (xbits.is_inf_or_nan()) {
      return x;
    }
    // Normalize denormal inputs: convert the integer significand to float
    // exactly and account for the 2^-149 subnormal scale in e_x.
    xbits = FPBits(static_cast<float>(static_cast<int32_t>(x_u)));
    e_x -= 149;
  }

  e_x += xbits.get_biased_exponent();
  fputil::FloatFloat l2 =
      log2f_eval(log2f_range_reduction(e_x, xbits.get_mantissa()));

  // log10(2) in FloatFloat:
  //   hi = round(log10(2), SG, RN), lo = round(log10(2) - hi, SG, RN).
  constexpr fputil::FloatFloat LOG10_2 = {-0x1.ec10cp-27f, 0x1.344136p-2f};
  fputil::FloatFloat prod = fputil::exact_mult<float>(l2.hi, LOG10_2.hi);
  float lo = fputil::multiply_add(
      l2.hi, LOG10_2.lo, fputil::multiply_add(l2.lo, LOG10_2.hi, prod.lo));
  return prod.hi + lo;
}

} // namespace float_eval
} // namespace math
} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC___SUPPORT_MATH_LOG10F_FLOAT_EVAL_H
