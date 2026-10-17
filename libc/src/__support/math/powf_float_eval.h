//===----------------------------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Float-only evaluation implementation for powf(x, y).
///
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_MATH_POWF_FLOAT_EVAL_H
#define LLVM_LIBC_SRC___SUPPORT_MATH_POWF_FLOAT_EVAL_H

#include "src/__support/CPP/bit.h"
#include "src/__support/FPUtil/FEnvImpl.h"
#include "src/__support/FPUtil/FPBits.h"
#include "src/__support/FPUtil/double_double.h"
#include "src/__support/FPUtil/multiply_add.h"
#include "src/__support/FPUtil/nearest_integer.h"
#include "src/__support/common.h"
#include "src/__support/macros/config.h"
#include "src/__support/macros/optimization.h"
#include "src/__support/macros/properties/cpu_features.h"
#include "src/__support/math/exp2f_float_utils.h"
#include "src/__support/math/log2f_float_utils.h"
#include "src/__support/math/powf_utils.h"

namespace LIBC_NAMESPACE_DECL {
namespace math {
namespace float_eval {

namespace powf_internal = LIBC_NAMESPACE::math::powf_internal;

// Overview of float-precision pow(x, y) = x^y computation:
//
// Let x = 2^(e_x) * m_x > 0.  Then:
//   x^y = 2^( y * log2(x) )
//       = 2^( y * ( e_x + log2(m_x) ) )
//       = 2^( k + rem )
//       = 2^k * 2^rem,
// where:
//   k = round(y * log2(x)),
//   rem = y * log2(x) - k.
//
// In particular, k is an integer, and |rem| <= 0.5.
//
// For the final result to fit in single precision, the exponent field is
// bounded by:
//   -150 <= y * log2(x) <= 128,
// and anything outside that range overflows or underflows.
//
// Since |rem| <= 0.5:
//   2^-0.5 <= 2^rem <= 2^0.5,
// the relative error of x^y = 2^k * 2^rem is approximately:
//   ~ absolute error of rem * ln(2)
//   ~ absolute error of y * log2(x) * ln(2) + rel_err(exp2f_eval)
//   ~ relative error(log2(x)) * |y * log2(x)| * ln(2) + 2^-25.
//
// With exp2f_eval providing error ~ 2^-25, the budget for
// relative error(log2(x)) * |y * log2(x)| * ln(2) is <= 2^-25.
// For single-precision results, the 8-bit exponent field bounds:
//   |y * log2(x)| < 2^8 = 256.
// Thus:
//   relative error(log2(x)) <= 2^-25 / (2^8 * ln(2)) < 2^-33.
//
// To compute log2(x) in FloatFloat precision with relative error < 2^-33, we
// use log2f_range_reduction and log2f_eval from log2f_float_utils.h, which
// perform a range reduction with a compact 16-entry lookup table followed by a
// degree-4 polynomial approximation.
//
// Finally, we compute the product y * log2(x) in FloatFloat:
//   y * log2(x) = y * ((e_x - log2(r)) + log2(1 + dx)).
// We decompose into integer k and fractional remainder rem:
//   k = round(total_hi),
//   rem = (total_hi - k) + total_lo,
// and evaluate 2^k * 2^rem via exp2f_eval(rem, k) from exp2f_float_utils.h,
// which provides a compact, code-size-friendly implementation without large
// exponent lookup tables.

LIBC_INLINE float powf(float x, float y) {
  using FPBits = fputil::FPBits<float>;
  using namespace powf_internal;
  using namespace log2f_float_internal;

  FPBits xbits(x), ybits(y);
  uint32_t x_u = xbits.uintval();
  uint32_t y_a = ybits.abs().uintval();

  int e_x = 0;
  uint32_t sign = 0;
  if (LIBC_UNLIKELY(y_a <= powf_internal::Y_LOWER_BOUND ||
                    y_a >= powf_internal::Y_UPPER_BOUND ||
                    x_u >= FPBits::inf().uintval() ||
                    x_u < FPBits::min_normal().uintval())) {
    if (auto r = powf_internal::check_exceptional_cases(x, y, e_x, sign);
        LIBC_UNLIKELY(r.has_value()))
      return r.value();
  }

  Sign out_sign = (sign == 0) ? Sign::POS : Sign::NEG;

  // Extract exponent field and mantissa of x.
  xbits = FPBits(x);
  e_x += xbits.get_biased_exponent() - FPBits::EXP_BIAS;
  uint32_t x_mant = xbits.get_mantissa();
  Log2fReduction red = log2f_range_reduction(e_x, x_mant);

  // Early overflow and underflow check:
  //   z = y * log2(x) ~ y * (e_x_hi + dx * log2(e))
  // With |dx| < 2^-4, |log2(1 + dx) - dx * log2(e)| <= (dx^2 / 2) * log2(e) <
  // 2^-8. For |y * log2(x)| <= 128, the error |z - y * log2(x)| < 4. Hence:
  //   z > 132  => overflow
  //   z < -155 => underflow
  float z = y * fputil::multiply_add(red.dx, LOG2_E.hi, red.e_x_hi);

  if (LIBC_UNLIKELY(z > 132.0f))
    return set_overflow(out_sign);

  if (LIBC_UNLIKELY(z < -155.0f))
    return set_underflow(out_sign);

  fputil::FloatFloat log2_x = log2f_eval(red);

  fputil::FloatFloat s = fputil::exact_mult<float>(y, log2_x.hi);
  float total_hi = s.hi;
  float total_lo = fputil::multiply_add(y, log2_x.lo, s.lo);

  // Exponent reduction for 2^(total_hi + total_lo):
  //   k = round(total_hi)
  //   rem = (total_hi - k) + total_lo, with |rem| <= 0.5.
  float kf = fputil::nearest_integer(total_hi);
  int k = static_cast<int>(kf);

  float rem_hi = total_hi - kf;
  float rem = rem_hi + total_lo;

  if (LIBC_UNLIKELY(k > 128))
    return set_overflow(out_sign);

  if (LIBC_UNLIKELY(k < -150))
    return set_underflow(out_sign);

  float result = exp2f_eval(rem, k);
  if (LIBC_UNLIKELY(k == 128 && FPBits(result).is_inf()))
    return set_overflow(out_sign);

  if (LIBC_UNLIKELY(k <= -126 && !FPBits(result).is_normal())) {
    fputil::set_errno_if_required(ERANGE);
    fputil::raise_underflow_except_if_required<float>();
  }

  return cpp::bit_cast<float>(cpp::bit_cast<uint32_t>(result) | sign);
}

} // namespace float_eval
} // namespace math
} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC___SUPPORT_MATH_POWF_FLOAT_EVAL_H
