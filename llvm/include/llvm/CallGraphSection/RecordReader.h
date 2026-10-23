//===- RecordReader.h - .llvm.callgraph record decoding ---------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Decodes the binary records of a `.llvm.callgraph` (SHT_LLVM_CALL_GRAPH)
// section into `FunctionRecord`s.
//
// Each address field in a record is reported both as the section-relative byte
// offset at which it was encoded and as the unsigned address value stored in
// that field. In a linked binary (`ET_EXEC` / `ET_DYN`), the stored value is
// the final virtual address of the function. In a relocatable object file
// (`ET_REL`), the caller maps the field offset through its relocation table to
// obtain the target symbol, using the stored value as the implicit addend when
// applicable.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CALLGRAPHSECTION_RECORDREADER_H
#define LLVM_CALLGRAPHSECTION_RECORDREADER_H

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/BinaryFormat/CallGraphSection.h"
#include "llvm/Support/DataExtractor.h"
#include "llvm/Support/Error.h"
#include <cstdint>

namespace llvm {

class Twine;

namespace callgraph {

/// A single decoded `.llvm.callgraph` record describing one function.
struct FunctionRecord {
  /// Raw `Flags` byte of the record.
  uint8_t RecordFlags = None;

  /// Section-relative offset of the function entry PC field.
  uint64_t FunctionAddressOffset = 0;

  /// Raw address value stored in the function entry PC field. In a linked
  /// binary, this is the virtual address of the function.
  uint64_t FunctionAddress = 0;

  /// Generalized function type ID, or 0 if unknown / not an indirect target.
  uint64_t FunctionTypeId = 0;

  /// Section-relative offsets of each direct callee PC field.
  SmallVector<uint64_t, 4> DirectCalleeAddressOffsets;

  /// Raw address values stored in each direct callee PC field. In a linked
  /// binary, these are the virtual addresses of the direct callees.
  SmallVector<uint64_t, 4> DirectCalleeAddresses;

  /// Type IDs of the signatures this function calls indirectly.
  SmallVector<uint64_t, 4> IndirectCalleeTypeIds;

  bool isIndirectTarget() const { return RecordFlags & IsIndirectTarget; }
  bool hasDirectCallees() const { return RecordFlags & HasDirectCallees; }
  bool hasIndirectCallees() const { return RecordFlags & HasIndirectCallees; }

  void clear() {
    RecordFlags = None;
    FunctionAddressOffset = 0;
    FunctionAddress = 0;
    FunctionTypeId = 0;
    DirectCalleeAddressOffsets.clear();
    DirectCalleeAddresses.clear();
    IndirectCalleeTypeIds.clear();
  }
};

/// Sequentially decodes the records of one `.llvm.callgraph` section.
///
/// Typical use:
/// \code
///   RecordReader Reader(Contents, IsLittleEndian, AddressSize);
///   FunctionRecord Record;
///   while (!Reader.atEnd()) {
///     if (Error E = Reader.readRecord(Record)) { /* diagnose */ break; }
///     // use Record.FunctionAddress (linked) or Record.FunctionAddressOffset
///   }
/// \endcode
class RecordReader {
public:
  /// \param Data           Raw contents of the section.
  /// \param IsLittleEndian Endianness of the containing object file.
  /// \param AddressSize    Size in bytes of an encoded target address.
  RecordReader(ArrayRef<uint8_t> Data, bool IsLittleEndian,
               uint8_t AddressSize);

  /// True once all records have been consumed, or decoding has failed.
  bool atEnd() const;

  /// Offset of the next record, useful for diagnostics.
  uint64_t tell() const;

  /// Decode the next record into \p Record.
  ///
  /// Returns an error for a truncated section, an unrecognized format version,
  /// or unknown flag bits. On error the reader becomes exhausted (`atEnd()` is
  /// true), so a caller that chooses to warn rather than fail will terminate
  /// its loop.
  Error readRecord(FunctionRecord &Record);

private:
  /// Abandon decoding and produce an error carrying \p Msg.
  Error fail(const Twine &Msg);

  DataExtractor Extractor;
  DataExtractor::Cursor Cursor;
  uint64_t Size;
  uint8_t AddressSize;
  bool Failed = false;
};

} // namespace callgraph
} // namespace llvm

#endif // LLVM_CALLGRAPHSECTION_RECORDREADER_H
