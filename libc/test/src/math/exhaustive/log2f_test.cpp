//===-- Exhaustive test for log2f -----------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "exhaustive_test.h"
#include "src/__support/math/log2f_float_eval.h"
#include "src/math/log2f.h"
#include "utils/MPFRWrapper/MPFRUtils.h"

namespace mpfr = LIBC_NAMESPACE::testing::mpfr;

using LlvmLibcLog2fExhaustiveTest =
    LlvmLibcUnaryOpExhaustiveMathTest<float, mpfr::Operation::Log2,
                                      LIBC_NAMESPACE::log2f>;

// Range: [0, Inf];
static constexpr uint32_t POS_START = 0x0000'0000U;
static constexpr uint32_t POS_STOP = 0x7f80'0000U;

TEST_F(LlvmLibcLog2fExhaustiveTest, PositiveRange) {
  test_full_range_all_roundings(POS_START, POS_STOP);
}

// Check the float-only implementation in all rounding modes, with a 1.5 ULP
// MPFR bound.
static float log2f_float_eval(float x) {
  return LIBC_NAMESPACE::math::float_eval::log2f(x);
}

using LlvmLibcLog2fFloatExhaustiveTest =
    LlvmLibcUnaryOpExhaustiveMathTest<float, mpfr::Operation::Log2,
                                      log2f_float_eval, 1>;

TEST_F(LlvmLibcLog2fFloatExhaustiveTest, PositiveRange) {
  test_full_range_all_roundings(POS_START, POS_STOP);
}
