//===-- Unittests for log2f -----------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "hdr/math_macros.h"
#include "src/__support/FPUtil/FPBits.h"
#include "src/__support/libc_errno.h"
#include "src/__support/macros/optimization.h"
#include "src/__support/math/log2f_double_eval.h"
#include "src/__support/math/log2f_float_eval.h"
#include "src/math/log2f.h"
#include "test/UnitTest/FPMatcher.h"
#include "test/UnitTest/Test.h"
#include "utils/MPFRWrapper/MPFRUtils.h"

#include "hdr/stdint_proxy.h"

#ifdef LIBC_MATH_HAS_SKIP_ACCURATE_PASS
#define TOLERANCE 1
#else
#define TOLERANCE 0
#endif // LIBC_MATH_HAS_SKIP_ACCURATE_PASS

namespace mpfr = LIBC_NAMESPACE::testing::mpfr;

class LlvmLibcLog2fTest : public LIBC_NAMESPACE::testing::FPTest<float> {
public:
  void test_eval_in_float_range(float (*func)(float), double tolerance) {
    constexpr uint32_t COUNT = 1'231;
    constexpr uint32_t STEP = UINT32_MAX / COUNT;
    for (uint32_t i = 0, v = 0; i <= COUNT; ++i, v += STEP) {
      float x = FPBits(v).get_val();
      if (FPBits(v).is_nan() || FPBits(v).is_inf() || FPBits(v).is_neg() ||
          FPBits(v).is_zero())
        continue;
      ASSERT_MPFR_MATCH_ALL_ROUNDING(mpfr::Operation::Log2, x, func(x),
                                     tolerance);
    }
  }
};

TEST_F(LlvmLibcLog2fTest, SpecialNumbers) {
  EXPECT_FP_EQ(aNaN, LIBC_NAMESPACE::log2f(aNaN));
  EXPECT_FP_EQ(inf, LIBC_NAMESPACE::log2f(inf));
  EXPECT_FP_IS_NAN_WITH_EXCEPTION(LIBC_NAMESPACE::log2f(neg_inf), FE_INVALID);
  EXPECT_FP_EQ_WITH_EXCEPTION(neg_inf, LIBC_NAMESPACE::log2f(0.0f),
                              FE_DIVBYZERO);
  EXPECT_FP_EQ_WITH_EXCEPTION(neg_inf, LIBC_NAMESPACE::log2f(-0.0f),
                              FE_DIVBYZERO);
  EXPECT_FP_IS_NAN_WITH_EXCEPTION(LIBC_NAMESPACE::log2f(-1.0f), FE_INVALID);
  EXPECT_FP_EQ_ALL_ROUNDING(zero, LIBC_NAMESPACE::log2f(1.0f));
}

TEST_F(LlvmLibcLog2fTest, TrickyInputs) {
  constexpr int N = 10;
  constexpr uint32_t INPUTS[N] = {
      0x3f7d57f5U, 0x3f7e3274U, 0x3f7ed848U, 0x3f7fd6ccU, 0x3f7fffffU,
      0x3f80079bU, 0x3f81d0b5U, 0x3f82e602U, 0x3f83c98dU, 0x3f8cba39U};

  for (int i = 0; i < N; ++i) {
    float x = FPBits(INPUTS[i]).get_val();
    EXPECT_MPFR_MATCH_ALL_ROUNDING(mpfr::Operation::Log2, x,
                                   LIBC_NAMESPACE::log2f(x), TOLERANCE + 0.5);
  }
}

TEST_F(LlvmLibcLog2fTest, InFloatRange) {
  constexpr uint32_t COUNT = 1'231;
  constexpr uint32_t STEP = UINT32_MAX / COUNT;
  for (uint32_t i = 0, v = 0; i <= COUNT; ++i, v += STEP) {
    float x = FPBits(v).get_val();
    if (FPBits(v).is_nan() || FPBits(v).is_inf())
      continue;
    libc_errno = 0;
    float result = LIBC_NAMESPACE::log2f(x);
    // If the computation resulted in an error or did not produce valid result
    // in the single-precision floating point range, then ignore comparing with
    // MPFR result as MPFR can still produce valid results because of its
    // wider precision.
    if (FPBits(result).is_nan() || FPBits(result).is_inf() || libc_errno != 0)
      continue;
    ASSERT_MPFR_MATCH_ALL_ROUNDING(mpfr::Operation::Log2, x,
                                   LIBC_NAMESPACE::log2f(x), TOLERANCE + 0.5);
  }
}

// Exercise both implementations independently of the configured selector.
TEST_F(LlvmLibcLog2fTest, DoubleEvalInFloatRange) {
  test_eval_in_float_range(&LIBC_NAMESPACE::math::double_eval::log2f, 0.5);
}

TEST_F(LlvmLibcLog2fTest, FloatEvalInFloatRange) {
  test_eval_in_float_range(&LIBC_NAMESPACE::math::float_eval::log2f, 1.5);
}
