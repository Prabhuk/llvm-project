//===-- Definition of EFI_LOADED_IMAGE_PROTOCOL type ----------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_TYPES_EFI_LOADED_IMAGE_PROTOCOL_H
#define LLVM_LIBC_TYPES_EFI_LOADED_IMAGE_PROTOCOL_H

#include "../llvm-libc-macros/stdint-macros.h"
#include "EFI_BOOT_SERVICES.h"
#include "EFI_DEVICE_PATH_PROTOCOL.h"
#include "EFI_HANDLE.h"
#include "EFI_MEMORY_TYPE.h"
#include "EFI_SYSTEM_TABLE.h"

#define EFI_LOADED_IMAGE_PROTOCOL_GUID                                         \
  {0x5b1b31a1, 0x9562, 0x11d2, {0x8e, 0x3f, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b}}

#define EFI_LOADED_IMAGE_PROTOCOL_REVISION 0x1000

typedef struct {
  uint32_t Revision;
  EFI_HANDLE ParentHandle;
  EFI_SYSTEM_TABLE *SystemTable;

  // Source location of the image
  EFI_HANDLE DeviceHandle;
  EFI_DEVICE_PATH_PROTOCOL *FilePath;
  void *Reserved;

  // Image's load options
  uint32_t LoadOptionsSize;
  void *LoadOptions;

  // Location where image was loaded
  void *ImageBase;
  uint64_t ImageSize;
  EFI_MEMORY_TYPE ImageCodeType;
  EFI_MEMORY_TYPE ImageDataType;
  EFI_IMAGE_UNLOAD Unload;
} EFI_LOADED_IMAGE_PROTOCOL;

#endif // LLVM_LIBC_TYPES_EFI_LOADED_IMAGE_PROTOCOL_H
