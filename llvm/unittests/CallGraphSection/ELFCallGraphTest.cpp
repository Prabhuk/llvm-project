//===- ELFCallGraphTest.cpp - Tests for ELF call graph extraction ---------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "llvm/CallGraphSection/ELFCallGraph.h"
#include "llvm/Object/ELFObjectFile.h"
#include "llvm/ObjectYAML/yaml2obj.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/YAMLTraits.h"
#include "llvm/Testing/Support/Error.h"
#include "gtest/gtest.h"

using namespace llvm;
using namespace llvm::callgraph;
using namespace llvm::object;

namespace {

static Expected<std::unique_ptr<ObjectFile>>
createELFObject(SmallVectorImpl<char> &Storage, StringRef Yaml) {
  raw_svector_ostream OS(Storage);
  yaml::Input YIn(Yaml);
  if (!yaml::convertYAML(YIn, OS, [](const Twine &) {}))
    return createStringError(std::errc::invalid_argument,
                             "unable to convert YAML");
  return ObjectFile::createELFObjectFile(MemoryBufferRef(OS.str(), "test.elf"));
}

TEST(ELFCallGraphTest, RejectsRelocatableObjectFile) {
  SmallString<0> Storage;
  auto ObjOrErr = createELFObject(Storage, R"(
--- !ELF
FileHeader:
  Class:   ELFCLASS64
  Data:    ELFDATA2LSB
  Type:    ET_REL
  Machine: EM_X86_64
)");
  ASSERT_THAT_EXPECTED(ObjOrErr, Succeeded());
  const auto &ELFObj = cast<ELFObjectFileBase>(**ObjOrErr);
  EXPECT_THAT_EXPECTED(ELFCallGraph::create(ELFObj),
                       FailedWithMessage(testing::HasSubstr(
                           "relocatable object file; link it first")));
}

TEST(ELFCallGraphTest, ExtractsCallGraphStackSizesAndICF) {
  // Layout:
  //   0x1000: caller (stack size 32), calls 0x2000 (direct), 0x0 (external),
  //           and indirect type IDs 0xAAA (resolved -> 0x3000) and 0xBBB
  //           (unresolved).
  //   0x2000: icf_func_a / icf_func_b (two .stack_sizes entries: 16 and 48;
  //           leaf on AArch64 -> max is 48).
  //   0x3000: target_fn (no .stack_sizes entry -> std::nullopt; indirect
  //           target with TypeId 0xAAA).
  //   0x4000: untyped_target (indirect target with TypeId 0 -> unknown type).
  SmallString<0> Storage;
  auto ObjOrErr = createELFObject(Storage, R"(
--- !ELF
FileHeader:
  Class:   ELFCLASS64
  Data:    ELFDATA2LSB
  Type:    ET_DYN
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
    # 0x1000 -> 32 (0x20), 0x2000 -> 16 (0x10), 0x2000 -> 48 (0x30)
    Content: "001000000000000020002000000000000010002000000000000030"
  - Name: .llvm.callgraph
    Type: SHT_LLVM_CALL_GRAPH
    # Record 1 (caller @ 0x1000):
    #   Ver=0, Flags=0x06 (Direct|Indirect), Entry=0x1000, TypeId=0,
    #   NumDirect=2, Callees=[0x2000, 0x0],
    #   NumIndirect=2, TypeIds=[0xAAA, 0xBBB]
    # Record 2 (icf_func @ 0x2000):
    #   Ver=0, Flags=0x00, Entry=0x2000, TypeId=0
    # Record 3 (target_fn @ 0x3000):
    #   Ver=0, Flags=0x01 (IsIndirectTarget), Entry=0x3000, TypeId=0xAAA
    # Record 4 (untyped_target @ 0x4000):
    #   Ver=0, Flags=0x01 (IsIndirectTarget), Entry=0x4000, TypeId=0
    Content: "000600100000000000000000000000000000020020000000000000000000000000000002aa0a000000000000bb0b00000000000000000020000000000000000000000000000000010030000000000000aa0a000000000000000100400000000000000000000000000000"
Symbols:
  - Name:    caller
    Type:    STT_FUNC
    Section: .text
    Value:   0x1000
  - Name:    icf_func_a
    Type:    STT_FUNC
    Section: .text
    Value:   0x2000
  - Name:    icf_func_b
    Type:    STT_FUNC
    Section: .text
    Value:   0x2000
  - Name:    target_fn
    Type:    STT_FUNC
    Section: .text
    Value:   0x3000
  - Name:    untyped_target
    Type:    STT_FUNC
    Section: .text
    Value:   0x4000
DynamicSymbols:
  - Name:    caller
    Type:    STT_FUNC
    Section: .text
    Value:   0x1000
)");
  ASSERT_THAT_EXPECTED(ObjOrErr, Succeeded());
  const auto &ELFObj = cast<ELFObjectFileBase>(**ObjOrErr);
  auto CGOrErr = ELFCallGraph::create(ELFObj);
  ASSERT_THAT_EXPECTED(CGOrErr, Succeeded());
  const ELFCallGraph &CG = *CGOrErr;

  EXPECT_EQ(CG.getCallFrameOverhead(), 0u);
  EXPECT_EQ(CG.getRedZoneSize(), 0u);
  EXPECT_EQ(formatSymbolizerMarkupPC(0x1000), "{{{pc:0x1000}}}");
  EXPECT_EQ(CG.getElfEntryPoint(), std::optional<uint64_t>(0x1000));
  EXPECT_EQ(CG.getDynamicFunctionAddresses(), ArrayRef<uint64_t>(0x1000));
  EXPECT_EQ(CG.getUnknownTypeIndirectTargets(), ArrayRef<uint64_t>(0x4000));

  const ELFFunctionInfo *Caller = CG.findFunction(0x1000);
  ASSERT_NE(Caller, nullptr);
  EXPECT_EQ(Caller->PrimaryName, "caller");
  EXPECT_EQ(Caller->StackSize, std::optional<uint64_t>(32));
  EXPECT_EQ(Caller->DirectCallees, ArrayRef<uint64_t>(0x2000));
  EXPECT_EQ(Caller->UninstrumentedCallees, ArrayRef<uint64_t>(0x0));
  EXPECT_EQ(CG.getUninstrumentedCalleeName(0x0), "<external>");
  ASSERT_EQ(Caller->IndirectCalls.size(), 1u);
  EXPECT_EQ(Caller->IndirectCalls[0].TypeId, 0xAAAu);
  EXPECT_EQ(Caller->IndirectCalls[0].TargetAddresses,
            ArrayRef<uint64_t>(0x3000));
  EXPECT_EQ(Caller->UnresolvedIndirectTypeIds, ArrayRef<uint64_t>(0xBBB));

  const ELFFunctionInfo *ICF = CG.findFunction(0x2000);
  ASSERT_NE(ICF, nullptr);
  EXPECT_EQ(ICF->PrimaryName, "icf_func_a");
  EXPECT_EQ(ICF->Names, ArrayRef<std::string>({"icf_func_a", "icf_func_b"}));
  EXPECT_EQ(ICF->StackSize, std::optional<uint64_t>(48));

  const ELFFunctionInfo *Target = CG.findFunction(0x3000);
  ASSERT_NE(Target, nullptr);
  EXPECT_EQ(Target->PrimaryName, "target_fn");
  EXPECT_EQ(Target->StackSize, std::nullopt);
}

TEST(ELFCallGraphTest, AppliesX86_64RedZoneToLeavesAndClearsARMThumbBit) {
  // Part 1: x86_64 leaf function gets +128 red zone; non-leaf does not.
  {
    SmallString<0> Storage;
    auto ObjOrErr = createELFObject(Storage, R"(
--- !ELF
FileHeader:
  Class:   ELFCLASS64
  Data:    ELFDATA2LSB
  Type:    ET_EXEC
  Machine: EM_X86_64
Sections:
  - Name: .text
    Type: SHT_PROGBITS
    Flags: [ SHF_ALLOC, SHF_EXECINSTR ]
    Address: 0x1000
    Size: 0x2000
  - Name: .stack_sizes
    Type: SHT_PROGBITS
    # 0x1000 -> 16, 0x2000 -> 24
    Content: "001000000000000010002000000000000018"
  - Name: .llvm.callgraph
    Type: SHT_LLVM_CALL_GRAPH
    # 0x1000 calls 0x2000; 0x2000 is a leaf.
    Content: "000200100000000000000000000000000000010020000000000000000000200000000000000000000000000000"
Symbols:
  - Name:    non_leaf
    Type:    STT_FUNC
    Section: .text
    Value:   0x1000
  - Name:    leaf
    Type:    STT_FUNC
    Section: .text
    Value:   0x2000
)");
    ASSERT_THAT_EXPECTED(ObjOrErr, Succeeded());
    auto CGOrErr = ELFCallGraph::create(cast<ELFObjectFileBase>(**ObjOrErr));
    ASSERT_THAT_EXPECTED(CGOrErr, Succeeded());
    EXPECT_EQ(CGOrErr->getCallFrameOverhead(), 8u);
    EXPECT_EQ(CGOrErr->getRedZoneSize(), 128u);
    EXPECT_EQ(CGOrErr->findFunction(0x1000)->StackSize,
              std::optional<uint64_t>(16));
    EXPECT_EQ(CGOrErr->findFunction(0x2000)->StackSize,
              std::optional<uint64_t>(24 + 128));
  }

  // Part 2: EM_ARM clears bit 0 (Thumb bit) on e_entry, symbols, .stack_sizes,
  // and .llvm.callgraph addresses.
  {
    SmallString<0> Storage;
    auto ObjOrErr = createELFObject(Storage, R"(
--- !ELF
FileHeader:
  Class:   ELFCLASS32
  Data:    ELFDATA2LSB
  Type:    ET_EXEC
  Machine: EM_ARM
  Entry:   0x1001
Sections:
  - Name: .text
    Type: SHT_PROGBITS
    Flags: [ SHF_ALLOC, SHF_EXECINSTR ]
    Address: 0x1000
    Size: 0x2000
  - Name: .stack_sizes
    Type: SHT_PROGBITS
    # 0x1001 -> 12, 0x2001 -> 8
    Content: "011000000c0120000008"
  - Name: .llvm.callgraph
    Type: SHT_LLVM_CALL_GRAPH
    # 0x1001 calls 0x2001; 0x2001 is a leaf.
    Content: "000201100000000000000000000001012000000000012000000000000000000000"
Symbols:
  - Name:    thumb_caller
    Type:    STT_FUNC
    Section: .text
    Value:   0x1001
  - Name:    thumb_leaf
    Type:    STT_FUNC
    Section: .text
    Value:   0x2001
)");
    ASSERT_THAT_EXPECTED(ObjOrErr, Succeeded());
    auto CGOrErr = ELFCallGraph::create(cast<ELFObjectFileBase>(**ObjOrErr));
    ASSERT_THAT_EXPECTED(CGOrErr, Succeeded());
    EXPECT_EQ(CGOrErr->getElfEntryPoint(), std::optional<uint64_t>(0x1000));
    const ELFFunctionInfo *Caller = CGOrErr->findFunction(0x1000);
    ASSERT_NE(Caller, nullptr);
    EXPECT_EQ(Caller->PrimaryName, "thumb_caller");
    EXPECT_EQ(Caller->StackSize, std::optional<uint64_t>(12));
    EXPECT_EQ(Caller->DirectCallees, ArrayRef<uint64_t>(0x2000));
    const ELFFunctionInfo *Leaf = CGOrErr->findFunction(0x2000);
    ASSERT_NE(Leaf, nullptr);
    EXPECT_EQ(Leaf->PrimaryName, "thumb_leaf");
    EXPECT_EQ(Leaf->StackSize, std::optional<uint64_t>(8));
  }
}

TEST(ELFCallGraphTest, ExcludesFunctionsCleanly) {
  SmallString<0> Storage;
  auto ObjOrErr = createELFObject(Storage, R"(
--- !ELF
FileHeader:
  Class:   ELFCLASS64
  Data:    ELFDATA2LSB
  Type:    ET_EXEC
  Machine: EM_AARCH64
Sections:
  - Name: .text
    Type: SHT_PROGBITS
    Flags: [ SHF_ALLOC, SHF_EXECINSTR ]
    Address: 0x1000
    Size: 0x2000
  - Name: .llvm.callgraph
    Type: SHT_LLVM_CALL_GRAPH
    # 0x1000 calls 0x2000; 0x2000 is excluded.
    Content: "000200100000000000000000000000000000010020000000000000000000200000000000000000000000000000"
Symbols:
  - Name:    main_fn
    Type:    STT_FUNC
    Section: .text
    Value:   0x1000
  - Name:    ignored_fn
    Type:    STT_FUNC
    Section: .text
    Value:   0x2000
)");
  ASSERT_THAT_EXPECTED(ObjOrErr, Succeeded());
  ELFCallGraphOptions Opts;
  Opts.ExcludedFunctions.push_back("ignored_fn");
  auto CGOrErr =
      ELFCallGraph::create(cast<ELFObjectFileBase>(**ObjOrErr), Opts);
  ASSERT_THAT_EXPECTED(CGOrErr, Succeeded());

  EXPECT_EQ(CGOrErr->findFunction(0x2000), nullptr);
  const ELFFunctionInfo *Main = CGOrErr->findFunction(0x1000);
  ASSERT_NE(Main, nullptr);
  EXPECT_TRUE(Main->DirectCallees.empty());
  EXPECT_TRUE(Main->UninstrumentedCallees.empty());
}

} // namespace
