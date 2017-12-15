/** @file
*
*  Copyright (c) 2015-2016, Linaro Ltd. All rights reserved.
*  Copyright (c) 2015-2016, Hisilicon Ltd. All rights reserved.
*
*  This program and the accompanying materials
*  are licensed and made available under the terms and conditions of the BSD License
*  which accompanies this distribution.  The full text of the license may be found at
*  http://opensource.org/licenses/bsd-license.php
*
*  THE PROGRAM IS DISTRIBUTED UNDER THE BSD LICENSE ON AN "AS IS" BASIS,
*  WITHOUT WARRANTIES OR REPRESENTATIONS OF ANY KIND, EITHER EXPRESS OR IMPLIED.
*
**/

#include <Library/BaseMemoryLib.h>
#include <Library/CacheMaintenanceLib.h>
#include <Library/DebugLib.h>
#include <Library/DevicePathLib.h>
#include <Library/IoLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/NonDiscoverableDeviceRegistrationLib.h>
#include <Library/PrintLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>
#include <Library/UefiRuntimeServicesTableLib.h>

#include <Protocol/Abootimg.h>
#include <Protocol/BlockIo.h>
#include <Protocol/EmbeddedGpio.h>
#include <Protocol/PlatformVirtualKeyboard.h>

#include <Hi3798cv200.h>
#include <libfdt.h>

#define SERIAL_NUMBER_SIZE               17
#define SERIAL_NUMBER_BLOCK_SIZE         EFI_PAGE_SIZE
#define SERIAL_NUMBER_LBA                8191
#define RANDOM_MAGIC                     0x9A4DBEAF

#define ADB_REBOOT_ADDRESS              0x08000000
#define ADB_REBOOT_BOOTLOADER           0x77665500
#define ADB_REBOOT_NONE                 0x77665501

typedef struct {
  UINT64        Magic;
  UINT64        Data;
  CHAR16        UnicodeSN[SERIAL_NUMBER_SIZE];
} RANDOM_SERIAL_NUMBER;

STATIC
EFI_STATUS
PoplarInitPeripherals (
  IN VOID
  )
{
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
AbootimgAppendKernelArgs (
  IN CHAR16            *Args,
  IN UINTN              Size
  )
{
  EFI_STATUS                  Status;
  EFI_BLOCK_IO_PROTOCOL      *BlockIoProtocol;
  VOID                       *DataPtr;
  RANDOM_SERIAL_NUMBER       *RandomSN;
  EFI_DEVICE_PATH_PROTOCOL   *FlashDevicePath;
  EFI_HANDLE                  FlashHandle;

  if (Args == NULL) {
    return EFI_INVALID_PARAMETER;
  }
  FlashDevicePath = ConvertTextToDevicePath ((CHAR16*)FixedPcdGetPtr (PcdAndroidFastbootNvmDevicePath));
  Status = gBS->LocateDevicePath (&gEfiBlockIoProtocolGuid, &FlashDevicePath, &FlashHandle);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "Warning: Couldn't locate Android NVM device (status: %r)\n", Status));
    // Failing to locate partitions should not prevent to do other Android FastBoot actions
    return EFI_SUCCESS;
  }
  Status = gBS->OpenProtocol (
                  FlashHandle,
                  &gEfiBlockIoProtocolGuid,
                  (VOID **) &BlockIoProtocol,
                  gImageHandle,
                  NULL,
                  EFI_OPEN_PROTOCOL_GET_PROTOCOL
                  );
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_WARN, "Warning: Couldn't open block device (status: %r)\n", Status));
    return EFI_DEVICE_ERROR;
  }

  DataPtr = AllocatePages (1);
  if (DataPtr == NULL) {
    return EFI_BUFFER_TOO_SMALL;
  }
  Status = BlockIoProtocol->ReadBlocks (
                              BlockIoProtocol,
                              BlockIoProtocol->Media->MediaId,
                              SERIAL_NUMBER_LBA,
                              SERIAL_NUMBER_BLOCK_SIZE,
                              DataPtr
                              );
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_WARN, "Warning: Failed on reading blocks\n"));
    goto Exit;
  }
  RandomSN = (RANDOM_SERIAL_NUMBER *)DataPtr;
  if (RandomSN->Magic != RANDOM_MAGIC) {
    UnicodeSPrint(
      RandomSN->UnicodeSN, SERIAL_NUMBER_SIZE * sizeof (CHAR16),
      L"0123456789abcdef"
      );
  }
  UnicodeSPrint (
    Args + StrLen (Args), Size - StrLen (Args),
    L" androidboot.serialno=%s",
    RandomSN->UnicodeSN
    );
  FreePages (DataPtr, 1);
  return EFI_SUCCESS;
Exit:
  FreePages (DataPtr, 1);
  return Status;
}

STATIC
EFI_STATUS
EFIAPI
AbootimgUpdateDtb (
  IN  EFI_PHYSICAL_ADDRESS        OrigFdtBase,
  OUT EFI_PHYSICAL_ADDRESS       *NewFdtBase
  )
{
  UINTN             FdtSize, NumPages;
  INTN              err;
  EFI_STATUS        Status;

  //
  // Sanity checks on the original FDT blob.
  //
  err = fdt_check_header ((VOID*)(UINTN)OrigFdtBase);
  if (err != 0) {
    DEBUG ((DEBUG_ERROR, "ERROR: Device Tree header not valid (err:%d)\n", err));
    return EFI_INVALID_PARAMETER;
  }

  //
  // Store the FDT as Runtime Service Data to prevent the Kernel from
  // overwritting its data.
  //
  FdtSize = fdt_totalsize ((VOID *)(UINTN)OrigFdtBase);
  NumPages = EFI_SIZE_TO_PAGES (FdtSize) + 20;
  Status = gBS->AllocatePages (
                  AllocateAnyPages, EfiRuntimeServicesData,
                  NumPages, NewFdtBase);
  if (EFI_ERROR (Status)) {
    return EFI_BUFFER_TOO_SMALL;
  }

  CopyMem (
    (VOID*)(UINTN)*NewFdtBase,
    (VOID*)(UINTN)OrigFdtBase,
    FdtSize
    );

  fdt_pack ((VOID *) (UINTN) *NewFdtBase);
  err = fdt_check_header ((VOID *) (UINTN) *NewFdtBase);
  if (err != 0) {
    DEBUG ((DEBUG_ERROR, "ERROR: Device Tree header not valid (err:%d)\n", err));
    gBS->FreePages (*NewFdtBase, NumPages);
    return EFI_INVALID_PARAMETER;
  }

  return EFI_SUCCESS;
}

STATIC ABOOTIMG_PROTOCOL mAbootimg = {
  AbootimgAppendKernelArgs,
  AbootimgUpdateDtb
};

STATIC
EFI_STATUS
EFIAPI
VirtualKeyboardRegister (
  IN VOID
  )
{
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
VirtualKeyboardReset (
  IN VOID
  )
{
  return EFI_SUCCESS;
}

STATIC
BOOLEAN
EFIAPI
VirtualKeyboardQuery (
  IN VIRTUAL_KBD_KEY             *VirtualKey
  )
{
  if (VirtualKey == NULL) {
    return FALSE;
  }

  if (MmioRead32 (ADB_REBOOT_ADDRESS) == ADB_REBOOT_BOOTLOADER) {
    VirtualKey->Signature = VIRTUAL_KEYBOARD_KEY_SIGNATURE;
    VirtualKey->Key.ScanCode = SCAN_NULL;
    VirtualKey->Key.UnicodeChar = L'f';
  }

  return TRUE;
}

STATIC
EFI_STATUS
EFIAPI
VirtualKeyboardClear (
  IN VIRTUAL_KBD_KEY            *VirtualKey
  )
{
  if (VirtualKey == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  if (MmioRead32 (ADB_REBOOT_ADDRESS) == ADB_REBOOT_BOOTLOADER) {
    MmioWrite32 (ADB_REBOOT_ADDRESS, ADB_REBOOT_NONE);
    WriteBackInvalidateDataCacheRange ((VOID *)ADB_REBOOT_ADDRESS, 4);
  }

  return EFI_SUCCESS;
}

STATIC PLATFORM_VIRTUAL_KBD_PROTOCOL mVirtualKeyboard = {
  VirtualKeyboardRegister,
  VirtualKeyboardReset,
  VirtualKeyboardQuery,
  VirtualKeyboardClear
};

EFI_STATUS
EFIAPI
PoplarEntryPoint (
  IN EFI_HANDLE         ImageHandle,
  IN EFI_SYSTEM_TABLE   *SystemTable
  )
{
  EFI_STATUS            Status;

  Status = PoplarInitPeripherals ();
  if (EFI_ERROR (Status)) {
    return Status;
  }

  // RegisterNonDicoverableMmioDevice
  Status = RegisterNonDiscoverableMmioDevice (
             NonDiscoverableDeviceTypeSdhci,
             NonDiscoverableDeviceDmaTypeNonCoherent,
             NULL,
             NULL,
             1,
             HI3798CV200_SDIO2_BASE, // eMMC
             SIZE_4KB
             );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = gBS->InstallProtocolInterface (
                  &ImageHandle,
                  &gAbootimgProtocolGuid,
                  EFI_NATIVE_INTERFACE,
                  &mAbootimg
                  );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = gBS->InstallProtocolInterface (
                  &ImageHandle,
                  &gPlatformVirtualKeyboardProtocolGuid,
                  EFI_NATIVE_INTERFACE,
                  &mVirtualKeyboard
                  );
  return Status;
}
