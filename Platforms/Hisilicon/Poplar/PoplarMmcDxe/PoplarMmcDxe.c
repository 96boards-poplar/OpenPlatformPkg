/** @file
*
*  Copyright (c) 2017, Linaro. All rights reserved.
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

#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/DevicePathLib.h>
#include <Library/IoLib.h>
#include <Library/TimerLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>
#include <Library/UefiRuntimeServicesTableLib.h>
#include <Library/UsbSerialNumberLib.h>

#include <Protocol/PlatformDwMmc.h>

STATIC DW_MMC_HC_SLOT_CAP DwMmcCapability = {
    .Ddr50       = 1,
    .HighSpeed   = 1,
    .BusWidth    = 8,
    .SlotType    = EmbeddedSlot,
    .CardType    = EmmcCardType,
    .BaseClkFreq = 100000
};

STATIC
EFI_STATUS
EFIAPI
PoplarGetCapability (
  IN     EFI_HANDLE           Controller,
  IN     UINT8                Slot,
     OUT DW_MMC_HC_SLOT_CAP   *Capability
  )
{
  if (Capability == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  DwMmcCapability.Controller = Controller;
  CopyMem (Capability, &DwMmcCapability, sizeof (DW_MMC_HC_SLOT_CAP));

  return EFI_SUCCESS;
}

STATIC
BOOLEAN
EFIAPI
PoplarCardDetect (
  IN UINT8                    Slot
  )
{
  return TRUE;
}

PLATFORM_DW_MMC_PROTOCOL mDwMmcDevice = {
  PoplarGetCapability,
  PoplarCardDetect
};

EFI_STATUS
EFIAPI
PoplarMmcEntryPoint (
  IN EFI_HANDLE                            ImageHandle,
  IN EFI_SYSTEM_TABLE                      *SystemTable
  )
{
  EFI_STATUS        Status;

  Status = gBS->InstallProtocolInterface (
                  &ImageHandle,
                  &gPlatformDwMmcProtocolGuid,
                  EFI_NATIVE_INTERFACE,
                  &mDwMmcDevice
                  );
  if (EFI_ERROR (Status)) {
    return Status;
  }
  return Status;
}
