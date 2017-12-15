/** @file
*
*  Copyright (c) 2015-2017, Linaro. All rights reserved.
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
#include <Library/DebugLib.h>
#include <Library/DevicePathLib.h>
#include <Library/IoLib.h>
#include <Library/TimerLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>
#include <Library/UefiRuntimeServicesTableLib.h>
#include <Library/UsbSerialNumberLib.h>

#include <Protocol/EmbeddedGpio.h>
#include <Protocol/DwUsb.h>

#include <Hi3798cv200.h>

// Last sector of bootloader partition (mmcsda1)
#define SERIAL_NUMBER_LBA        8191

STATIC
VOID
PoparInnoUsbPhyConfig (
  VOID
  )
{
  UINT32        Configs[] = {
                  // Close EOP pre-emphasis, open data pre-emphasis
                  0xA1001C,
                  // Rcomp = 150mW, increase DC level
                  0xA00607,
                  // Keep Rcomp working */
                  0xA10700,
                  // Icomp = 212mW, increase current drive
                  0xA00AAB,
                  // EMI fix: rx_active not stay 1 when error packets received
                  0xA11140,
                  // Comp mode select
                  0xA11041,
  };
  UINTN         Index;

  for (Index = 0; Index < ARRAY_SIZE (Configs); Index++) {
    MmioWrite32 (HI3798CV200_PERI_USB0, Configs[Index]);
    MmioWrite32 (HI3798CV200_PERI_USB0, Configs[Index] | USB2_PHY01_TEST_CLK);
    MmioWrite32 (HI3798CV200_PERI_USB0, Configs[Index]);
    MicroSecondDelay (20);
  }
}

STATIC
VOID
PoplarDwUSBPhyInit (
  IN VOID
  )
{
  STATIC BOOLEAN        Initialized = FALSE;
  UINT32                Value;

  if (Initialized) {
    return;
  }

  // Reset usb2 controller bus/utmi/roothub
  Value = MmioRead32 (HI3798CV200_CRG46);
  Value |= USB2_BUS_SRST_REQ | USB2_UTMI_SRST_REQ |
           USB2_HST_PHY_SRST_REQ | USB2_OTG_PHY_SRST_REQ;
  MmioWrite32 (HI3798CV200_CRG46, Value);
  MicroSecondDelay (200);

  // Reset usb2 phy por/utmi
  Value = MmioRead32 (HI3798CV200_CRG47);
  Value |= USB2_PHY01_SRST_REQ | USB2_PHY01_SRST_TREQ1 | USB2_PHY01_SRST_TREQ0;
  MmioWrite32 (HI3798CV200_CRG47, Value);
  MicroSecondDelay (200);

  // Open usb2 ref clk
  Value = MmioRead32 (HI3798CV200_CRG47);
  Value |= USB2_PHY01_REF_CKEN;
  MmioWrite32 (HI3798CV200_CRG47, Value);
  MicroSecondDelay (300);

  // Cancel usb2 power on reset
  Value = MmioRead32 (HI3798CV200_CRG47);
  Value &= ~USB2_PHY01_SRST_REQ;
  MmioWrite32 (HI3798CV200_CRG47, Value);
  MicroSecondDelay (500);

  // Configure Inno USB Phy
  PoparInnoUsbPhyConfig();

  // Cancel usb2 port reset, wait comp circuit stable
  Value = MmioRead32 (HI3798CV200_CRG47);
  Value &= ~(USB2_PHY01_SRST_TREQ1 | USB2_PHY01_SRST_TREQ0);
  MmioWrite32 (HI3798CV200_CRG47, Value);
  MicroSecondDelay (2000);

  // Open usb2 controller
  Value = MmioRead32 (HI3798CV200_CRG46);
  Value |= USB2_BUS_CKEN | USB2_OHCI48M_CKEN | USB2_OHCI12M_CKEN |
           USB2_OTG_UTMI_CKEN | USB2_HST_PHY_CKEN | USB2_UTMI_CKEN;
  MmioWrite32 (HI3798CV200_CRG46, Value);
  MicroSecondDelay (200);

  // Cancel control reset */
  Value = MmioRead32 (HI3798CV200_CRG46);
  Value &= ~(USB2_BUS_SRST_REQ | USB2_UTMI_SRST_REQ |
             USB2_HST_PHY_SRST_REQ | USB2_OTG_PHY_SRST_REQ);
  MmioWrite32 (HI3798CV200_CRG46, Value);
  MicroSecondDelay (200);

  Initialized = TRUE;
}


EFI_STATUS
PoplarUsbPhyInit (
  IN UINT8        Mode
  )
{
  UINTN         Value;

  Value = MmioRead32 (HI3798CV200_PERI_USB3);
  if (Mode == USB_HOST_MODE) {
    Value &= ~USB2_2P_CHIPID;
  } else {
    Value |= USB2_2P_CHIPID;
  }
  MmioWrite32 (HI3798CV200_PERI_USB3, Value);

  PoplarDwUSBPhyInit();

  return EFI_SUCCESS;
}

EFI_STATUS
EFIAPI
PoplarUsbGetLang (
  OUT CHAR16            *Lang,
  OUT UINT8             *Length
  )
{
  if ((Lang == NULL) || (Length == NULL)) {
    return EFI_INVALID_PARAMETER;
  }
  Lang[0] = 0x409;
  *Length = sizeof (CHAR16);
  return EFI_SUCCESS;
}

EFI_STATUS
EFIAPI
PoplarUsbGetManuFacturer (
  OUT CHAR16            *ManuFacturer,
  OUT UINT8             *Length
  )
{
  UINTN                  VariableSize;
  CHAR16                 DataUnicode[MANU_FACTURER_STRING_LENGTH];

  if ((ManuFacturer == NULL) || (Length == NULL)) {
    return EFI_INVALID_PARAMETER;
  }
  VariableSize = MANU_FACTURER_STRING_LENGTH * sizeof (CHAR16);
  ZeroMem (DataUnicode, MANU_FACTURER_STRING_LENGTH * sizeof(CHAR16));
  StrCpy (DataUnicode, L"96Boards");
  CopyMem (ManuFacturer, DataUnicode, VariableSize);
  *Length = VariableSize;
  return EFI_SUCCESS;
}

EFI_STATUS
EFIAPI
PoplarUsbGetProduct (
  OUT CHAR16            *Product,
  OUT UINT8             *Length
  )
{
  UINTN                  VariableSize;
  CHAR16                 DataUnicode[PRODUCT_STRING_LENGTH];

  if ((Product == NULL) || (Length == NULL)) {
    return EFI_INVALID_PARAMETER;
  }
  VariableSize = PRODUCT_STRING_LENGTH * sizeof (CHAR16);
  ZeroMem (DataUnicode, PRODUCT_STRING_LENGTH * sizeof(CHAR16));
  StrCpy (DataUnicode, L"Poplar");
  CopyMem (Product, DataUnicode, VariableSize);
  *Length = VariableSize;
  return EFI_SUCCESS;
}

EFI_STATUS
EFIAPI
PoplarUsbGetSerialNo (
  OUT CHAR16            *SerialNo,
  OUT UINT8             *Length
  )
{
  EFI_STATUS                          Status;
  EFI_DEVICE_PATH_PROTOCOL           *FlashDevicePath;
  EFI_HANDLE                          FlashHandle;

  FlashDevicePath = ConvertTextToDevicePath ((CHAR16*)FixedPcdGetPtr (PcdAndroidFastbootNvmDevicePath));
  Status = gBS->LocateDevicePath (&gEfiBlockIoProtocolGuid, &FlashDevicePath, &FlashHandle);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "Warning: Couldn't locate Android NVM device (status: %r)\n", Status));
    // Failing to locate partitions should not prevent to do other Android FastBoot actions
    return EFI_SUCCESS;
  }

  if ((SerialNo == NULL) || (Length == NULL)) {
    return EFI_INVALID_PARAMETER;
  }
  Status = LoadSNFromBlock (FlashHandle, SERIAL_NUMBER_LBA, SerialNo);
  *Length = StrSize (SerialNo);
  return Status;
}

DW_USB_PROTOCOL mDwUsbDevice = {
  PoplarUsbGetLang,
  PoplarUsbGetManuFacturer,
  PoplarUsbGetProduct,
  PoplarUsbGetSerialNo,
  PoplarUsbPhyInit
};

EFI_STATUS
EFIAPI
PoplarUsbEntryPoint (
  IN EFI_HANDLE                            ImageHandle,
  IN EFI_SYSTEM_TABLE                      *SystemTable
  )
{
  EFI_STATUS        Status;

  Status = gBS->InstallProtocolInterface (
                  &ImageHandle,
                  &gDwUsbProtocolGuid,
                  EFI_NATIVE_INTERFACE,
                  &mDwUsbDevice
                  );
  if (EFI_ERROR (Status)) {
    return Status;
  }
  return Status;
}
