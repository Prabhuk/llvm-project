//===----------------------------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Float-only implementation of logf.
///
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_MATH_LOGF_FLOAT_EVAL_H
#define LLVM_LIBC_SRC___SUPPORT_MATH_LOGF_FLOAT_EVAL_H

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

// Float-only logf:
//   logf(x) = log2(x) * LN2,
// where log2(x) is evaluated in FloatFloat precision with relative error
// < 2^-33 by log2f_float_utils.h (shared with powf and log2f), and the product
// with LN2 is computed in FloatFloat before the final rounding.
LIBC_INLINE float logf(float x) {
  using FPBits = fputil::FPBits<float>;
  using namespace log2f_float_internal;

  FPBits xbits(x);
  uint32_t x_u = xbits.uintval();
  int e_x = -FPBits::EXP_BIAS;

  // logf(1.0f) = +0.0f in all rounding modes.
  if (LIBC_UNLIKELY(x_u == 0x3f80'0000U))
    return 0.0f;

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
      if (xbits.is_signaling_nan()) {
        fputil::raise_except_if_required(FE_INVALID);
        return FPBits::quiet_nan().get_val();
      }
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

  // ln(2) in FloatFloat:
  //   hi = round(log(2), SG, RN), lo = round(log(2) - hi, SG, RN).
  constexpr fputil::FloatFloat LN2 = {-0x1.05c61p-29f, 0x1.62e43p-1f};
  fputil::FloatFloat prod = fputil::exact_mult<float>(l2.hi, LN2.hi);
  float lo = fputil::multiply_add(l2.hi, LN2.lo,
                                  fputil::multiply_add(l2.lo, LN2.hi, prod.lo));
  return prod.hi + lo;
}

} // namespace float_eval
} // namespace math
} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC___SUPPORT_MATH_LOGF_FLOAT_EVAL_H
