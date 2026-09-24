//===- RecordReaderTest.cpp - Tests for .llvm.callgraph decoding ----------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "llvm/CallGraphSection/RecordReader.h"
#include "llvm/Support/Error.h"
#include "llvm/Testing/Support/Error.h"
#include "gtest/gtest.h"
#include <vector>

using namespace llvm;
using namespace llvm::callgraph;

namespace {

/// Builds a little-endian, 64-bit `.llvm.callgraph` section body.
class SectionBuilder {
public:
  void u8(uint8_t V) { Bytes.push_back(V); }
  void u64(uint64_t V) {
    for (unsigned I = 0; I != 8; ++I)
      Bytes.push_back(static_cast<uint8_t>(V >> (8 * I)));
  }
  /// Single-byte ULEB128; sufficient for values below 128.
  void uleb(uint8_t V) { Bytes.push_back(V); }
  size_t size() const { return Bytes.size(); }
  ArrayRef<uint8_t> get() const { return Bytes; }

private:
  std::vector<uint8_t> Bytes;
};

TEST(RecordReaderTest, DecodesRecordAndReportsAddressOffsets) {
  SectionBuilder S;
  S.u8(V_0);
  S.u8(IsIndirectTarget | HasDirectCallees | HasIndirectCallees);
  const size_t EntryPCOffset = S.size();
  S.u64(0); // entry PC placeholder, supplied by a relocation
  S.u64(0x1234);
  S.uleb(1);
  const size_t CalleePCOffset = S.size();
  S.u64(0); // direct callee PC placeholder
  S.uleb(1);
  S.u64(0xDEAD);

  RecordReader Reader(S.get(), /*IsLittleEndian=*/true, /*AddressSize=*/8);
  FunctionRecord Record;

  ASSERT_FALSE(Reader.atEnd());
  ASSERT_THAT_ERROR(Reader.readRecord(Record), Succeeded());

  EXPECT_TRUE(Record.isIndirectTarget());
  EXPECT_TRUE(Record.hasDirectCallees());
  EXPECT_TRUE(Record.hasIndirectCallees());
  EXPECT_EQ(Record.FunctionAddressOffset, EntryPCOffset);
  EXPECT_EQ(Record.FunctionTypeId, 0x1234u);
  ASSERT_EQ(Record.DirectCalleeAddressOffsets.size(), 1u);
  EXPECT_EQ(Record.DirectCalleeAddressOffsets[0], CalleePCOffset);
  ASSERT_EQ(Record.IndirectCalleeTypeIds.size(), 1u);
  EXPECT_EQ(Record.IndirectCalleeTypeIds[0], 0xDEADu);
  EXPECT_TRUE(Reader.atEnd());
}

TEST(RecordReaderTest, DecodesConsecutiveRecords) {
  SectionBuilder S;
  for (unsigned I = 0; I != 2; ++I) {
    S.u8(V_0);
    S.u8(IsIndirectTarget);
    S.u64(0);
    S.u64(0x40 + I);
  }

  RecordReader Reader(S.get(), /*IsLittleEndian=*/true, /*AddressSize=*/8);
  FunctionRecord Record;

  ASSERT_THAT_ERROR(Reader.readRecord(Record), Succeeded());
  EXPECT_EQ(Record.FunctionTypeId, 0x40u);
  ASSERT_FALSE(Reader.atEnd());
  ASSERT_THAT_ERROR(Reader.readRecord(Record), Succeeded());
  EXPECT_EQ(Record.FunctionTypeId, 0x41u);
  EXPECT_TRUE(Reader.atEnd());
}

TEST(RecordReaderTest, RejectsUnsupportedVersion) {
  SectionBuilder S;
  S.u8(LatestFormatVersion + 1);
  S.u8(0);
  S.u64(0);
  S.u64(0);

  RecordReader Reader(S.get(), /*IsLittleEndian=*/true, /*AddressSize=*/8);
  FunctionRecord Record;
  EXPECT_THAT_ERROR(Reader.readRecord(Record), Failed());
  // A failed reader reports itself exhausted so that callers which only warn
  // still terminate their loop.
  EXPECT_TRUE(Reader.atEnd());
}

TEST(RecordReaderTest, RejectsTruncatedRecord) {
  SectionBuilder S;
  S.u8(V_0);
  S.u8(IsIndirectTarget);
  S.u64(0);
  // Type ID field is missing.

  RecordReader Reader(S.get(), /*IsLittleEndian=*/true, /*AddressSize=*/8);
  FunctionRecord Record;
  EXPECT_THAT_ERROR(Reader.readRecord(Record), Failed());
}

TEST(RecordReaderTest, RejectsImplausibleCalleeCount) {
  SectionBuilder S;
  S.u8(V_0);
  S.u8(HasDirectCallees);
  S.u64(0);
  S.u64(0);
  S.uleb(120); // 120 callees claimed, none present

  RecordReader Reader(S.get(), /*IsLittleEndian=*/true, /*AddressSize=*/8);
  FunctionRecord Record;
  EXPECT_THAT_ERROR(Reader.readRecord(Record), Failed());
  // Nothing was reserved or appended for the bogus count.
  EXPECT_TRUE(Record.DirectCalleeAddressOffsets.empty());
}

} // namespace
