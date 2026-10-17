//===-- Exhaustive test for logf ------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "exhaustive_test.h"
#include "src/__support/math/logf_float_eval.h"
#include "src/math/logf.h"
#include "utils/MPFRWrapper/MPFRUtils.h"

namespace mpfr = LIBC_NAMESPACE::testing::mpfr;

using LlvmLibcLogfExhaustiveTest =
    LlvmLibcUnaryOpExhaustiveMathTest<float, mpfr::Operation::Log,
                                      LIBC_NAMESPACE::logf>;

// Range: [0, Inf];
static constexpr uint32_t POS_START = 0x0000'0000U;
static constexpr uint32_t POS_STOP = 0x7f80'0000U;

TEST_F(LlvmLibcLogfExhaustiveTest, PositiveRange) {
  test_full_range_all_roundings(POS_START, POS_STOP);
}

// Check the float-only implementation in all rounding modes, with a 1.5 ULP
// MPFR bound.
static float logf_float_eval(float x) {
  return LIBC_NAMESPACE::math::float_eval::logf(x);
}

using LlvmLibcLogfFloatExhaustiveTest =
    LlvmLibcUnaryOpExhaustiveMathTest<float, mpfr::Operation::Log,
                                      logf_float_eval, 1>;

TEST_F(LlvmLibcLogfFloatExhaustiveTest, PositiveRange) {
  test_full_range_all_roundings(POS_START, POS_STOP);
}
