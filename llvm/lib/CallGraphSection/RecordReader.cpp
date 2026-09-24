//===- RecordReader.cpp - .llvm.callgraph record decoding -----------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "llvm/CallGraphSection/RecordReader.h"
#include "llvm/ADT/Twine.h"
#include <algorithm>

using namespace llvm;
using namespace llvm::callgraph;

RecordReader::RecordReader(ArrayRef<uint8_t> Data, bool IsLittleEndian,
                           uint8_t AddressSize)
    : Extractor(Data, IsLittleEndian), Cursor(0), Size(Data.size()),
      AddressSize(AddressSize) {}

bool RecordReader::atEnd() const { return Failed || Cursor.tell() >= Size; }

uint64_t RecordReader::tell() const { return Cursor.tell(); }

/// Abandon decoding and report \p Msg. The cursor's own error, if any, is
/// consumed here so that callers only have to handle the returned Error.
Error RecordReader::fail(const Twine &Msg) {
  Failed = true;
  consumeError(Cursor.takeError());
  return createStringError(inconvertibleErrorCode(), Msg);
}

Error RecordReader::readRecord(FunctionRecord &Record) {
  Record.clear();

  if (atEnd())
    return fail("read past the end of the call graph section");

  const uint64_t RecordStart = Cursor.tell();

  uint8_t Version = Extractor.getU8(Cursor);
  if (!Cursor)
    return fail("truncated call graph record at offset " + Twine(RecordStart));
  if (Version > LatestFormatVersion)
    return fail("unsupported call graph section format version " +
                Twine(static_cast<unsigned>(Version)) + " at offset " +
                Twine(RecordStart));

  Record.RecordFlags = Extractor.getU8(Cursor);

  // The entry PC is relocated; report where it lives rather than its value.
  Record.FunctionAddressOffset = Cursor.tell();
  Extractor.getUnsigned(Cursor, AddressSize);

  Record.FunctionTypeId = Extractor.getU64(Cursor);
  if (!Cursor)
    return fail("truncated call graph record at offset " + Twine(RecordStart));

  // Element counts come from the input, so validate them against the bytes
  // actually remaining before reserving or looping. A corrupt or hostile
  // object file must not be able to induce a huge allocation here.
  auto readCount = [&](uint64_t ElementSize,
                       const char *What) -> Expected<uint64_t> {
    uint64_t Count = Extractor.getULEB128(Cursor);
    if (!Cursor)
      return fail(Twine("truncated ") + What + " count at offset " +
                  Twine(RecordStart));
    uint64_t Remaining = Size > Cursor.tell() ? Size - Cursor.tell() : 0;
    if (Count > Remaining / ElementSize)
      return fail(Twine("call graph record at offset ") + Twine(RecordStart) +
                  " declares " + Twine(Count) + " " + What +
                  " but the section has only " + Twine(Remaining) +
                  " bytes remaining");
    return Count;
  };

  if (Record.hasDirectCallees()) {
    Expected<uint64_t> Count =
        readCount(std::max<uint8_t>(AddressSize, 1), "direct callees");
    if (!Count)
      return Count.takeError();
    Record.DirectCalleeAddressOffsets.reserve(*Count);
    for (uint64_t I = 0; I != *Count; ++I) {
      Record.DirectCalleeAddressOffsets.push_back(Cursor.tell());
      Extractor.getUnsigned(Cursor, AddressSize);
    }
  }

  if (Record.hasIndirectCallees()) {
    Expected<uint64_t> Count =
        readCount(sizeof(uint64_t), "indirect callee type IDs");
    if (!Count)
      return Count.takeError();
    Record.IndirectCalleeTypeIds.reserve(*Count);
    for (uint64_t I = 0; I != *Count; ++I)
      Record.IndirectCalleeTypeIds.push_back(Extractor.getU64(Cursor));
  }

  if (!Cursor)
    return fail("truncated call graph record at offset " + Twine(RecordStart));

  return Error::success();
}
