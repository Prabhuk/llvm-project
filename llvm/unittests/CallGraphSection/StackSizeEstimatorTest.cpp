//===- StackSizeEstimatorTest.cpp - Tests for stack usage estimation ------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "llvm/CallGraphSection/StackSizeEstimator.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/BinaryFormat/CallGraphSection.h"
#include "llvm/Object/ELFObjectFile.h"
#include "llvm/ObjectYAML/yaml2obj.h"
#include "llvm/Support/EndianStream.h"
#include "llvm/Support/LEB128.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/YAMLTraits.h"
#include "llvm/Testing/Support/Error.h"
#include "gtest/gtest.h"

using namespace llvm;
using namespace llvm::callgraph;
using namespace llvm::object;

namespace {

struct TestCGRecord {
  uint64_t Address = 0;
  bool IsIndirectTarget = false;
  uint64_t TypeId = 0;
  std::vector<uint64_t> DirectCallees;
  std::vector<uint64_t> IndirectTypeIds;
};

static std::string encodeCallGraphHex(ArrayRef<TestCGRecord> Records) {
  SmallVector<char, 256> Bytes;
  raw_svector_ostream OS(Bytes);
  support::endian::Writer W(OS, llvm::endianness::little);

  for (const TestCGRecord &R : Records) {
    Flags F = Flags::None;
    if (R.IsIndirectTarget)
      F |= Flags::IsIndirectTarget;
    if (!R.DirectCallees.empty())
      F |= Flags::HasDirectCallees;
    if (!R.IndirectTypeIds.empty())
      F |= Flags::HasIndirectCallees;

    W.write<uint8_t>(FormatVersion::V_0);
    W.write<uint8_t>(static_cast<uint8_t>(F));
    W.write<uint64_t>(R.Address);
    W.write<uint64_t>(R.IsIndirectTarget ? R.TypeId : 0);

    if (!R.DirectCallees.empty()) {
      encodeULEB128(R.DirectCallees.size(), OS);
      for (uint64_t Callee : R.DirectCallees)
        W.write<uint64_t>(Callee);
    }
    if (!R.IndirectTypeIds.empty()) {
      encodeULEB128(R.IndirectTypeIds.size(), OS);
      for (uint64_t TId : R.IndirectTypeIds)
        W.write<uint64_t>(TId);
    }
  }
  return toHex(ArrayRef<uint8_t>(
      reinterpret_cast<const uint8_t *>(Bytes.data()), Bytes.size()));
}

static std::string
encodeStackSizesHex(ArrayRef<std::pair<uint64_t, uint64_t>> Entries) {
  SmallVector<char, 128> Bytes;
  raw_svector_ostream OS(Bytes);
  support::endian::Writer W(OS, llvm::endianness::little);
  for (const auto &[Addr, Size] : Entries) {
    W.write<uint64_t>(Addr);
    encodeULEB128(Size, OS);
  }
  return toHex(ArrayRef<uint8_t>(
      reinterpret_cast<const uint8_t *>(Bytes.data()), Bytes.size()));
}

static Expected<std::unique_ptr<ObjectFile>>
createELFObject(SmallVectorImpl<char> &Storage, StringRef Yaml) {
  raw_svector_ostream OS(Storage);
  yaml::Input YIn(Yaml);
  if (!yaml::convertYAML(YIn, OS, [](const Twine &) {}))
    return createStringError(std::errc::invalid_argument,
                             "unable to convert YAML");
  return ObjectFile::createELFObjectFile(MemoryBufferRef(OS.str(), "test.elf"));
}

TEST(StackSizeEstimatorTest, DiamondDAGAndParetoPruning) {
  // Graph on AArch64 (call frame overhead = 0):
  //   entry (0x1000, 16B) -> left (0x2000, 64B) -> sink (0x4000, 8B)
  //                       -> right (0x3000, 32B) -> sink (0x4000, 8B)
  // Worst-case path: entry (16) -> left (64) -> sink (8) = 88 bytes.
  std::string StackHex = encodeStackSizesHex(
      {{0x1000, 16}, {0x2000, 64}, {0x3000, 32}, {0x4000, 8}});
  std::string CGHex = encodeCallGraphHex({
      {0x1000, false, 0, {0x2000, 0x3000}, {}},
      {0x2000, false, 0, {0x4000}, {}},
      {0x3000, false, 0, {0x4000}, {}},
      {0x4000, false, 0, {}, {}},
  });

  std::string Yaml = (Twine(R"(
--- !ELF
FileHeader:
  Class:   ELFCLASS64
  Data:    ELFDATA2LSB
  Type:    ET_EXEC
  Machine: EM_AARCH64
  Entry:   0x1000
Sections:
  - Name: .text
    Type: SHT_PROGBITS
    Flags: [ SHF_ALLOC, SHF_EXECINSTR ]
    Address: 0x1000
    Size: 0x4000
  - Name: .stack_sizes
    Type: SHT_PROGBITS
    Content: ")") + StackHex +
                      R"("
  - Name: .llvm.callgraph
    Type: SHT_LLVM_CALL_GRAPH
    Content: ")" + CGHex +
                      R"("
Symbols:
  - Name: entry
    Type: STT_FUNC
    Section: .text
    Value: 0x1000
  - Name: left
    Type: STT_FUNC
    Section: .text
    Value: 0x2000
  - Name: right
    Type: STT_FUNC
    Section: .text
    Value: 0x3000
  - Name: sink
    Type: STT_FUNC
    Section: .text
    Value: 0x4000
)")
                         .str();

  SmallString<0> Storage;
  auto ObjOrErr = createELFObject(Storage, Yaml);
  ASSERT_THAT_EXPECTED(ObjOrErr, Succeeded());
  auto CGOrErr = ELFCallGraph::create(cast<ELFObjectFileBase>(**ObjOrErr));
  ASSERT_THAT_EXPECTED(CGOrErr, Succeeded());

  auto ReportOrErr = estimateStackSizes(*CGOrErr);
  ASSERT_THAT_EXPECTED(ReportOrErr, Succeeded());
  ASSERT_EQ(ReportOrErr->EntryPoints.size(), 1u);

  const EntryPointEstimate &EP = ReportOrErr->EntryPoints[0];
  EXPECT_EQ(EP.Name, "entry");
  EXPECT_EQ(EP.MaxStackUsage, 88u);
  EXPECT_EQ(EP.StackUsageExpr, "88");
  EXPECT_FALSE(EP.IsLowerBound);
  ASSERT_EQ(EP.MaxStackPath.size(), 3u);
  EXPECT_EQ(EP.MaxStackPath[0].Name, "entry");
  EXPECT_EQ(EP.MaxStackPath[1].Name, "left");
  EXPECT_EQ(EP.MaxStackPath[2].Name, "sink");
}

TEST(StackSizeEstimatorTest, CyclesAndAlgebraicLowerBounds) {
  // Graph on AArch64:
  //   entry (0x1000, 16B) -> cycle_a (0x2000, 32B) <-> cycle_b (0x3000, 24B)
  //                       -> branch_unk (0x4000, unknown frame) -> ext (0x0)
  //                       -> branch_ind (0x5000, 80B, unresolved indirect 0x99)
  //
  // SCC(cycle_a, cycle_b) has local stack 32 + 24 = 56B and unknown
  // "cycle(cycle_a -> cycle_b)".
  // branch_unk has local stack 0B and unknowns "frame(branch_unk) +
  // calls(<external>)". branch_ind has local stack 80B and unknown
  // "indirect(branch_ind)". From entry (16B), the three branches are
  // incomparable in the Pareto order:
  //   - 96 + indirect(branch_ind)
  //   - 72 + cycle(cycle_a -> cycle_b)
  //   - 16 + frame(branch_unk) + calls(<external>)
  std::string StackHex = encodeStackSizesHex(
      {{0x1000, 16}, {0x2000, 32}, {0x3000, 24}, {0x5000, 80}});
  std::string CGHex = encodeCallGraphHex({
      {0x1000, false, 0, {0x2000, 0x4000, 0x5000}, {}},
      {0x2000, false, 0, {0x3000}, {}},
      {0x3000, false, 0, {0x2000}, {}},
      {0x4000, false, 0, {0x0}, {}},
      {0x5000, false, 0, {}, {0x99}},
  });

  std::string Yaml = (Twine(R"(
--- !ELF
FileHeader:
  Class:   ELFCLASS64
  Data:    ELFDATA2LSB
  Type:    ET_EXEC
  Machine: EM_AARCH64
  Entry:   0x1000
Sections:
  - Name: .text
    Type: SHT_PROGBITS
    Flags: [ SHF_ALLOC, SHF_EXECINSTR ]
    Address: 0x1000
    Size: 0x5000
  - Name: .stack_sizes
    Type: SHT_PROGBITS
    Content: ")") + StackHex +
                      R"("
  - Name: .llvm.callgraph
    Type: SHT_LLVM_CALL_GRAPH
    Content: ")" + CGHex +
                      R"("
Symbols:
  - Name: entry
    Type: STT_FUNC
    Section: .text
    Value: 0x1000
  - Name: cycle_a
    Type: STT_FUNC
    Section: .text
    Value: 0x2000
  - Name: cycle_b
    Type: STT_FUNC
    Section: .text
    Value: 0x3000
  - Name: branch_unk
    Type: STT_FUNC
    Section: .text
    Value: 0x4000
  - Name: branch_ind
    Type: STT_FUNC
    Section: .text
    Value: 0x5000
)")
                         .str();

  SmallString<0> Storage;
  auto ObjOrErr = createELFObject(Storage, Yaml);
  ASSERT_THAT_EXPECTED(ObjOrErr, Succeeded());
  auto CGOrErr = ELFCallGraph::create(cast<ELFObjectFileBase>(**ObjOrErr));
  ASSERT_THAT_EXPECTED(CGOrErr, Succeeded());

  auto ReportOrErr = estimateStackSizes(*CGOrErr, {}, "test.elf");
  ASSERT_THAT_EXPECTED(ReportOrErr, Succeeded());
  ASSERT_EQ(ReportOrErr->EntryPoints.size(), 1u);

  const EntryPointEstimate &EP = ReportOrErr->EntryPoints[0];
  EXPECT_EQ(EP.MaxStackUsage, 96u);
  EXPECT_TRUE(EP.IsLowerBound);
  EXPECT_EQ(EP.StackUsageExpr,
            "max(96 + indirect(branch_ind), 72 + cycle(cycle_a -> cycle_b), "
            "16 + frame(branch_unk) + calls(<external>))");
  ASSERT_EQ(EP.Cycles.size(), 1u);
  ASSERT_EQ(EP.UnknownStackSizes.size(), 1u);
  EXPECT_EQ(EP.UnknownStackSizes[0].Name, "branch_unk");
  ASSERT_EQ(EP.UninstrumentedCallees.size(), 1u);
  EXPECT_EQ(EP.UninstrumentedCallees[0].TargetName, "<external>");
  ASSERT_EQ(EP.UnresolvedIndirectCalls.size(), 1u);
  EXPECT_EQ(EP.UnresolvedIndirectCalls[0].CallerName, "branch_ind");

  // Verify JSON and text formatting.
  json::Value J = toJSON(*ReportOrErr);
  const json::Object *Root = J.getAsObject();
  ASSERT_NE(Root, nullptr);
  EXPECT_EQ(Root->getString("binary"), std::optional<StringRef>("test.elf"));

  std::string TextOut;
  raw_string_ostream TextOS(TextOut);
  formatTextReport(*ReportOrErr, TextOS);
  EXPECT_NE(TextOut.find("Entry Point: entry (0x1000)"), std::string::npos);
  EXPECT_NE(TextOut.find("[LOWER BOUND]"), std::string::npos);
  EXPECT_NE(TextOut.find("Cycle detected: cycle_a -> cycle_b"),
            std::string::npos);
}

TEST(StackSizeEstimatorTest, GlobAndMissingEntryPointDiagnostics) {
  std::string StackHex = encodeStackSizesHex({{0x1000, 16}, {0x2000, 32}});
  std::string CGHex = encodeCallGraphHex({
      {0x1000, false, 0, {}, {}},
      {0x2000, false, 0, {}, {}},
  });

  std::string Yaml = (Twine(R"(
--- !ELF
FileHeader:
  Class:   ELFCLASS64
  Data:    ELFDATA2LSB
  Type:    ET_DYN
  Machine: EM_AARCH64
Sections:
  - Name: .text
    Type: SHT_PROGBITS
    Flags: [ SHF_ALLOC, SHF_EXECINSTR ]
    Address: 0x1000
    Size: 0x3000
  - Name: .stack_sizes
    Type: SHT_PROGBITS
    Content: ")") + StackHex +
                      R"("
  - Name: .llvm.callgraph
    Type: SHT_LLVM_CALL_GRAPH
    Content: ")" + CGHex +
                      R"("
Symbols:
  - Name: api_one
    Type: STT_FUNC
    Section: .text
    Value: 0x1000
  - Name: api_two
    Type: STT_FUNC
    Section: .text
    Value: 0x2000
  - Name: dup_sym
    Type: STT_FUNC
    Section: .text
    Value: 0x1000
DynamicSymbols:
  - Name: dup_sym
    Type: STT_FUNC
    Section: .text
    Value: 0x2000
)")
                         .str();

  SmallString<0> Storage;
  auto ObjOrErr = createELFObject(Storage, Yaml);
  ASSERT_THAT_EXPECTED(ObjOrErr, Succeeded());
  auto CGOrErr = ELFCallGraph::create(cast<ELFObjectFileBase>(**ObjOrErr));
  ASSERT_THAT_EXPECTED(CGOrErr, Succeeded());

  StackEstimateOptions Opts;
  Opts.EntryPoints = {"api_*"};
  auto ReportOrErr = estimateStackSizes(*CGOrErr, Opts);
  ASSERT_THAT_EXPECTED(ReportOrErr, Succeeded());
  ASSERT_EQ(ReportOrErr->EntryPoints.size(), 2u);
  EXPECT_EQ(ReportOrErr->EntryPoints[0].Name, "api_one");
  EXPECT_EQ(ReportOrErr->EntryPoints[1].Name, "api_two");

  Opts.EntryPoints = {"nonexistent"};
  EXPECT_THAT_EXPECTED(
      estimateStackSizes(*CGOrErr, Opts),
      FailedWithMessage(testing::HasSubstr("entry function not found")));

  Opts.EntryPoints = {"dup_sym"};
  EXPECT_THAT_EXPECTED(
      estimateStackSizes(*CGOrErr, Opts),
      FailedWithMessage(testing::HasSubstr("ambiguous entry function name")));
}

} // namespace
