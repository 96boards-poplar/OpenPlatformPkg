/** @file

  Copyright (c) 2014, ARM Ltd. All rights reserved.<BR>
  Copyright (c) 2015-2017, Linaro. All rights reserved.

  This program and the accompanying materials
  are licensed and made available under the terms and conditions of the BSD License
  which accompanies this distribution.  The full text of the license may be found at
  http://opensource.org/licenses/bsd-license.php

  THE PROGRAM IS DISTRIBUTED UNDER THE BSD LICENSE ON AN "AS IS" BASIS,
  WITHOUT WARRANTIES OR REPRESENTATIONS OF ANY KIND, EITHER EXPRESS OR IMPLIED.

**/

/*
  Implementation of the Android Fastboot Platform protocol, to be used by the
  Fastboot UEFI application, for Hisilicon Poplar platform.
*/

#include <Protocol/AndroidFastbootPlatform.h>
#include <Protocol/BlockIo.h>
#include <Protocol/DiskIo.h>
#include <Protocol/EraseBlock.h>
#include <Protocol/SimpleTextOut.h>

#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/CacheMaintenanceLib.h>
#include <Library/DebugLib.h>
#include <Library/DevicePathLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/IoLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiRuntimeServicesTableLib.h>
#include <Library/UsbSerialNumberLib.h>
#include <Library/PrintLib.h>
#include <Library/TimerLib.h>

#include <IndustryStandard/Mbr.h>

#define PARTITION_NAME_MAX_LENGTH (72/2)

#define SERIAL_NUMBER_LBA                8191
#define RANDOM_MAX                       0x7FFFFFFFFFFFFFFF
#define RANDOM_MAGIC                     0x9A4DBEAF

#define ADB_REBOOT_ADDRESS               0x08000000
#define ADB_REBOOT_BOOTLOADER            0x77665500

#define MMC_BLOCK_SIZE                   512
#define POPLAR_ERASE_SIZE                4096

//
// Extract UINT32 from char array
//
#define UNPACK_UINT32(a) (UINT32)( (((UINT8 *) a)[0] <<  0) |    \
                                   (((UINT8 *) a)[1] <<  8) |    \
                                   (((UINT8 *) a)[2] << 16) |    \
                                   (((UINT8 *) a)[3] << 24) )

typedef struct _FASTBOOT_PARTITION_LIST {
  LIST_ENTRY  Link;
  CHAR16      PartitionName[PARTITION_NAME_MAX_LENGTH];
  EFI_LBA     StartingLBA;
  EFI_LBA     EndingLBA;
} FASTBOOT_PARTITION_LIST;

STATIC LIST_ENTRY                       mPartitionListHead;
STATIC EFI_HANDLE                       mFlashHandle;
STATIC EFI_BLOCK_IO_PROTOCOL           *mFlashBlockIo;
STATIC EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *mTextOut;

/*
  Helper to free the partition list
*/
STATIC
VOID
FreePartitionList (
  VOID
  )
{
  FASTBOOT_PARTITION_LIST *Entry;
  FASTBOOT_PARTITION_LIST *NextEntry;

  Entry = (FASTBOOT_PARTITION_LIST *) GetFirstNode (&mPartitionListHead);
  while (!IsNull (&mPartitionListHead, &Entry->Link)) {
    NextEntry = (FASTBOOT_PARTITION_LIST *) GetNextNode (&mPartitionListHead, &Entry->Link);

    RemoveEntryList (&Entry->Link);
    FreePool (Entry);

    Entry = NextEntry;
  }
}

STATIC
EFI_STATUS
AddPartitionRecordToList (
  IN  UINTN                     PartitionIndex,
  IN  EFI_LBA                   StartingLBA,
  IN  EFI_LBA                   EndingLBA
  )
{
  FASTBOOT_PARTITION_LIST *Entry;

  Entry = AllocatePool (sizeof (FASTBOOT_PARTITION_LIST));
  if (Entry == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }

  UnicodeSPrint (Entry->PartitionName, PARTITION_NAME_MAX_LENGTH, L"mmcsda%d", PartitionIndex + 1);
  Entry->StartingLBA = StartingLBA;
  Entry->EndingLBA = EndingLBA;
  InsertTailList (&mPartitionListHead, &Entry->Link);

  return EFI_SUCCESS;
}

STATIC
BOOLEAN
PartitionValidMbr (
  IN  MASTER_BOOT_RECORD        *Mbr,
  IN  EFI_LBA                   LastLBA
  )
{
  UINT32  StartingLBA;
  UINT32  EndingLBA;
  UINT32  NewEndingLBA;
  INTN    Index1;
  INTN    Index2;
  BOOLEAN MbrValid;

  if (Mbr->Signature != MBR_SIGNATURE) {
    return FALSE;
  }
  //
  // The BPB also has this signature, so it can not be used alone.
  //
  MbrValid = FALSE;
  for (Index1 = 0; Index1 < MAX_MBR_PARTITIONS; Index1++) {
    if (Mbr->Partition[Index1].OSIndicator == 0x00 || UNPACK_UINT32 (Mbr->Partition[Index1].SizeInLBA) == 0) {
      continue;
    }

    MbrValid    = TRUE;
    StartingLBA = UNPACK_UINT32 (Mbr->Partition[Index1].StartingLBA);
    EndingLBA   = StartingLBA + UNPACK_UINT32 (Mbr->Partition[Index1].SizeInLBA) - 1;
    if (EndingLBA > LastLBA) {
      //
      // Compatability Errata:
      //  Some systems try to hide drive space with thier INT 13h driver
      //  This does not hide space from the OS driver. This means the MBR
      //  that gets created from DOS is smaller than the MBR created from
      //  a real OS (NT & Win98). This leads to BlockIo->LastBlock being
      //  wrong on some systems FDISKed by the OS.
      //
      //  return FALSE Because no block devices on a system are implemented
      //  with INT 13h
      //
      return FALSE;
    }

    for (Index2 = Index1 + 1; Index2 < MAX_MBR_PARTITIONS; Index2++) {
      if (Mbr->Partition[Index2].OSIndicator == 0x00 || UNPACK_UINT32 (Mbr->Partition[Index2].SizeInLBA) == 0) {
        continue;
      }

      NewEndingLBA = UNPACK_UINT32 (Mbr->Partition[Index2].StartingLBA) +
                     UNPACK_UINT32 (Mbr->Partition[Index2].SizeInLBA) - 1;
      if (NewEndingLBA >= StartingLBA && UNPACK_UINT32 (Mbr->Partition[Index2].StartingLBA) <= EndingLBA) {
        //
        // This region overlaps with the Index1'th region
        //
        return FALSE;
      }
    }
  }
  //
  // Non of the regions overlapped so MBR is O.K.
  //
  return MbrValid;
}

STATIC
EFI_STATUS
PoplarReadExtPartitions (
  IN  EFI_BLOCK_IO_PROTOCOL     *BlockIo,
  IN  MBR_PARTITION_RECORD      *ExtPartition,
  OUT UINTN                     *PartitionNumbers
  )
{
  EFI_STATUS                    Status;
  UINT32                        MediaId;
  UINTN                         BlockSize;
  MASTER_BOOT_RECORD            *Mbr;
  MBR_PARTITION_RECORD          *Partition0;
  MBR_PARTITION_RECORD          *Partition1;
  EFI_LBA                       ExtPartitionBaseLBA;
  EFI_LBA                       NextEbrStartingLBA = 0;
  EFI_LBA                       StartingLBA;
  EFI_LBA                       EndingLBA;

  MediaId = BlockIo->Media->MediaId;
  BlockSize = BlockIo->Media->BlockSize;

  Mbr = AllocatePool (BlockSize);
  if (Mbr == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }

  ExtPartitionBaseLBA = UNPACK_UINT32 (ExtPartition->StartingLBA);

  //
  // Read all the Extended Partitions
  //
  do {
    NextEbrStartingLBA += ExtPartitionBaseLBA;
    Status = BlockIo->ReadBlocks (BlockIo, MediaId, NextEbrStartingLBA, BlockSize, (VOID *) Mbr);
    if (EFI_ERROR (Status)) {
      return Status;
    }

    if (Mbr->Signature != MBR_SIGNATURE) {
      break;
    }

    Partition0 = &Mbr->Partition[0];
    Partition1 = &Mbr->Partition[1];

    if (UNPACK_UINT32 (Partition0->StartingLBA) && UNPACK_UINT32 (Partition0->SizeInLBA)) {
      StartingLBA = NextEbrStartingLBA + UNPACK_UINT32 (Partition0->StartingLBA);
      EndingLBA = StartingLBA + UNPACK_UINT32 (Partition0->SizeInLBA) - 1;
      Status = AddPartitionRecordToList (*PartitionNumbers, StartingLBA, EndingLBA);
      if (EFI_ERROR (Status)) {
        return Status;
      }
      (*PartitionNumbers)++;
      NextEbrStartingLBA = UNPACK_UINT32 (Partition1->StartingLBA);
    } else {
      break;
    }
  } while (UNPACK_UINT32 (Partition1->StartingLBA) && UNPACK_UINT32 (Partition1->SizeInLBA));

  return Status;
}

/*
  Read the PartitionName fields from the MBR partition entries, putting them
  into an allocated array that should later be freed.
*/
STATIC
EFI_STATUS
ReadPartitionEntries (
  IN  EFI_BLOCK_IO_PROTOCOL *BlockIo
  )
{
  EFI_STATUS                    Status;
  UINT32                        MediaId;
  UINTN                         BlockSize;
  MASTER_BOOT_RECORD            *Mbr;
  MBR_PARTITION_RECORD          *Partition;
  EFI_LBA                       StartingLBA;
  EFI_LBA                       EndingLBA;
  UINTN                         PartitionNumbers = 0;
  UINTN                         Index;

  MediaId = BlockIo->Media->MediaId;
  BlockSize = BlockIo->Media->BlockSize;

  //
  // Read size of Partition entry and number of entries from MBR header
  //
  Mbr = AllocatePool (BlockSize);
  if (Mbr == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }

  Status = BlockIo->ReadBlocks (BlockIo, MediaId, 0, BlockSize, (VOID *) Mbr);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  //
  // Check there is a MBR on the media
  //
  if (!PartitionValidMbr (Mbr, BlockIo->Media->LastBlock)) {
    DEBUG ((EFI_D_ERROR,
      "Fastboot platform: No MBR on flash. "
      "Fastboot on Poplar does not support GPT.\n"
    ));
    return EFI_DEVICE_ERROR;
  }

  for (Index = 0; Index < MAX_MBR_PARTITIONS; Index++) {
    Partition = &Mbr->Partition[Index];

    if (Partition->OSIndicator == 0x00 || UNPACK_UINT32 (Partition->SizeInLBA) == 0) {
      continue;
    }

    if ((Partition->OSIndicator == EXTENDED_DOS_PARTITION) ||
        (Partition->OSIndicator == EXTENDED_WINDOWS_PARTITION)) {
      PartitionNumbers++;
      PoplarReadExtPartitions (BlockIo, Partition, &PartitionNumbers);
    } else {
      StartingLBA = UNPACK_UINT32 (Partition->StartingLBA);
      EndingLBA = StartingLBA + UNPACK_UINT32 (Partition->SizeInLBA) - 1;
      Status = AddPartitionRecordToList (PartitionNumbers++, StartingLBA, EndingLBA);
      if (EFI_ERROR (Status)) {
        return Status;
      }
    }
  }

  return EFI_SUCCESS;
}

/*
  Initialise: Open the Android NVM device and find the partitions on it. Save them in
  a list along with the "PartitionName" fields for their MBR entries.
  We will use these partition names as the key in PoplarFastbootPlatformFlashPartition.
*/
EFI_STATUS
PoplarFastbootPlatformInit (
  VOID
  )
{
  EFI_STATUS                            Status;
  EFI_DEVICE_PATH_PROTOCOL              *FlashDevicePath;
  EFI_DEVICE_PATH_PROTOCOL              *FlashDevicePathDup;

  InitializeListHead (&mPartitionListHead);

  Status = gBS->LocateProtocol (&gEfiSimpleTextOutProtocolGuid, NULL, (VOID **) &mTextOut);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR,
      "Fastboot platform: Couldn't open Text Output Protocol: %r\n", Status
      ));
    return Status;
  }

  //
  // Get EFI_HANDLES for all the partitions on the block devices pointed to by
  // PcdFastbootFlashDevicePath, also saving their MBR partition labels.
  // There's no way to find all of a device's children, so we get every handle
  // in the system supporting EFI_BLOCK_IO_PROTOCOL and then filter out ones
  // that don't represent partitions on the flash device.
  //
  FlashDevicePath = ConvertTextToDevicePath ((CHAR16*)FixedPcdGetPtr (PcdAndroidFastbootNvmDevicePath));

  // Create another device path pointer because LocateDevicePath will modify it.
  FlashDevicePathDup = FlashDevicePath;
  Status = gBS->LocateDevicePath (&gEfiBlockIoProtocolGuid, &FlashDevicePathDup, &mFlashHandle);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "Warning: Couldn't locate Android NVM device (status: %r)\n", Status));
    // Failing to locate partitions should not prevent to do other Android FastBoot actions
    return EFI_SUCCESS;
  }

  Status = gBS->OpenProtocol (
                  mFlashHandle,
                  &gEfiBlockIoProtocolGuid,
                  (VOID **) &mFlashBlockIo,
                  gImageHandle,
                  NULL,
                  EFI_OPEN_PROTOCOL_GET_PROTOCOL
                  );
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "Fastboot platform: Couldn't open Android NVM device (status: %r)\n", Status));
    return EFI_DEVICE_ERROR;
  }

  // Read the MBR partition entry array into memory so we can get the partition names
  Status = ReadPartitionEntries (mFlashBlockIo);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "Warning: Failed to read partitions from Android NVM device (status: %r)\n", Status));
    // Failing to locate partitions should not prevent to do other Android FastBoot actions
    return EFI_SUCCESS;
  }

  return Status;
}

VOID
PoplarFastbootPlatformUnInit (
  VOID
  )
{
  FreePartitionList ();
}

EFI_STATUS
PoplarFastbootPlatformFlashPartition (
  IN CHAR8  *PartitionName,
  IN UINTN   Size,
  IN VOID   *Image
  )
{
  EFI_STATUS               Status;
  UINTN                    PartitionSize;
  FASTBOOT_PARTITION_LIST *Entry;
  CHAR16                   PartitionNameUnicode[60];
  BOOLEAN                  PartitionFound;
  EFI_DISK_IO_PROTOCOL    *DiskIo;
  UINTN                    BlockSize;

  AsciiStrToUnicodeStr (PartitionName, PartitionNameUnicode);
  PartitionFound = FALSE;
  Entry = (FASTBOOT_PARTITION_LIST *) GetFirstNode (&(mPartitionListHead));
  while (!IsNull (&mPartitionListHead, &Entry->Link)) {
    // Search the partition list for the partition named by PartitionName
    if (StrCmp (Entry->PartitionName, PartitionNameUnicode) == 0) {
      PartitionFound = TRUE;
      break;
    }

   Entry = (FASTBOOT_PARTITION_LIST *) GetNextNode (&mPartitionListHead, &(Entry)->Link);
  }
  if (!PartitionFound) {
    return EFI_NOT_FOUND;
  }

  // Check image will fit on device
  BlockSize = mFlashBlockIo->Media->BlockSize;
  PartitionSize = (Entry->EndingLBA - Entry->StartingLBA + 1) * BlockSize;
  if (PartitionSize < Size) {
    DEBUG ((DEBUG_ERROR, "Partition not big enough.\n"));
    DEBUG ((DEBUG_ERROR, "Partition Size:\t%ld\nImage Size:\t%ld\n", PartitionSize, Size));

    return EFI_VOLUME_FULL;
  }
  Status = gBS->OpenProtocol (
                  mFlashHandle,
                  &gEfiDiskIoProtocolGuid,
                  (VOID **) &DiskIo,
                  gImageHandle,
                  NULL,
                  EFI_OPEN_PROTOCOL_GET_PROTOCOL
                  );
  ASSERT_EFI_ERROR (Status);

  Status = DiskIo->WriteDisk (
                     DiskIo,
                     mFlashBlockIo->Media->MediaId,
                     Entry->StartingLBA * BlockSize,
                     Size,
                     Image
                     );
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "Failed to write %d bytes into 0x%x, Status:%r\n", Size, Entry->StartingLBA * BlockSize, Status));
    return Status;
  }

  mFlashBlockIo->FlushBlocks(mFlashBlockIo);
  MicroSecondDelay (50000);

  return Status;
}

EFI_STATUS
PoplarFastbootPlatformErasePartition (
  IN CHAR8 *PartitionName
  )
{
  EFI_STATUS                  Status;
  EFI_ERASE_BLOCK_PROTOCOL   *EraseBlockProtocol;
  UINTN                       Size;
  BOOLEAN                     PartitionFound;
  CHAR16                      PartitionNameUnicode[60];
  FASTBOOT_PARTITION_LIST    *Entry;

  AsciiStrToUnicodeStr (PartitionName, PartitionNameUnicode);

  PartitionFound = FALSE;
  Entry = (FASTBOOT_PARTITION_LIST *) GetFirstNode (&mPartitionListHead);
  while (!IsNull (&mPartitionListHead, &Entry->Link)) {
    // Search the partition list for the partition named by PartitionName
    if (StrCmp (Entry->PartitionName, PartitionNameUnicode) == 0) {
      PartitionFound = TRUE;
      break;
    }
    Entry = (FASTBOOT_PARTITION_LIST *) GetNextNode (&mPartitionListHead, &Entry->Link);
  }
  if (!PartitionFound) {
    return EFI_NOT_FOUND;
  }

  Status = gBS->OpenProtocol (
                  mFlashHandle,
                  &gEfiEraseBlockProtocolGuid,
                  (VOID **) &EraseBlockProtocol,
                  gImageHandle,
                  NULL,
                  EFI_OPEN_PROTOCOL_GET_PROTOCOL
                  );
  if (EFI_ERROR (Status)) {
    return Status;
  }
  Size = (Entry->EndingLBA - Entry->StartingLBA + 1) * mFlashBlockIo->Media->BlockSize;
  Status = EraseBlockProtocol->EraseBlocks (
                                 EraseBlockProtocol,
                                 mFlashBlockIo->Media->MediaId,
                                 Entry->StartingLBA,
                                 NULL,
                                 Size
                                 );
  return Status;
}

EFI_STATUS
PoplarFastbootPlatformGetVar (
  IN  CHAR8   *Name,
  OUT CHAR8   *Value
  )
{
  EFI_STATUS               Status;
  UINT64                   PartitionSize;
  FASTBOOT_PARTITION_LIST *Entry;
  CHAR16                   PartitionNameUnicode[60];
  BOOLEAN                  PartitionFound;
  CHAR16                   UnicodeSN[SERIAL_NUMBER_SIZE];

  if (!AsciiStrCmp (Name, "max-download-size")) {
    AsciiStrCpy (Value, FixedPcdGetPtr (PcdArmFastbootFlashLimit));
  } else if (!AsciiStrCmp (Name, "product")) {
    AsciiStrCpy (Value, FixedPcdGetPtr (PcdFirmwareVendor));
  } else if (!AsciiStrCmp (Name, "serialno")) {
    Status = LoadSNFromBlock (mFlashHandle, SERIAL_NUMBER_LBA, UnicodeSN);
    if (EFI_ERROR (Status)) {
      *Value = '\0';
      return Status;
    }
    UnicodeStrToAsciiStr (UnicodeSN, Value);
  } else if ( !AsciiStrnCmp (Name, "partition-size", 14)) {
    AsciiStrToUnicodeStr ((Name + 15), PartitionNameUnicode);
    PartitionFound = FALSE;
    Entry = (FASTBOOT_PARTITION_LIST *) GetFirstNode (&(mPartitionListHead));
    while (!IsNull (&mPartitionListHead, &Entry->Link)) {
      // Search the partition list for the partition named by PartitionName
      if (StrCmp (Entry->PartitionName, PartitionNameUnicode) == 0) {
        PartitionFound = TRUE;
        break;
      }

     Entry = (FASTBOOT_PARTITION_LIST *) GetNextNode (&mPartitionListHead, &(Entry)->Link);
    }
    if (!PartitionFound) {
      *Value = '\0';
      return EFI_NOT_FOUND;
    }

    PartitionSize = (Entry->EndingLBA - Entry->StartingLBA + 1) * mFlashBlockIo->Media->BlockSize;
    DEBUG ((DEBUG_ERROR, "Fastboot platform: check for partition-size:%a 0X%llx\n", Name, PartitionSize));
    AsciiSPrint (Value, 12, "0x%llx", PartitionSize);
  } else if ( !AsciiStrCmp (Name, "erase-block-size")) {
    AsciiSPrint (Value, 12, "0x%llx", POPLAR_ERASE_SIZE);
  } else if ( !AsciiStrCmp (Name, "logical-block-size")) {
    AsciiSPrint (Value, 12, "0x%llx", POPLAR_ERASE_SIZE);
  } else {
    *Value = '\0';
  }
  return EFI_SUCCESS;
}

EFI_STATUS
PoplarFastbootPlatformOemCommand (
  IN  CHAR8   *Command
  )
{
  EFI_STATUS   Status;
  CHAR16       UnicodeSN[SERIAL_NUMBER_SIZE];

  if (AsciiStrCmp (Command, "Demonstrate") == 0) {
    DEBUG ((DEBUG_ERROR, "ARM OEM Fastboot command 'Demonstrate' received.\n"));
    return EFI_SUCCESS;
  } else if (AsciiStrCmp (Command, "serialno") == 0) {
    Status = GenerateUsbSN (UnicodeSN);
    if (EFI_ERROR (Status)) {
      DEBUG ((DEBUG_ERROR, "Failed to generate USB Serial Number.\n"));
      return Status;
    }
    Status = StoreSNToBlock (mFlashHandle, SERIAL_NUMBER_LBA, UnicodeSN);
    return Status;
  } else if (AsciiStrCmp (Command, "reboot-bootloader") == 0) {
    MmioWrite32 (ADB_REBOOT_ADDRESS, ADB_REBOOT_BOOTLOADER);
    WriteBackInvalidateDataCacheRange ((VOID *)ADB_REBOOT_ADDRESS, 4);
    return EFI_SUCCESS;
  } else {
    DEBUG ((DEBUG_ERROR,
      "Poplar: Unrecognised Fastboot OEM command: %s\n",
      Command
      ));
    return EFI_NOT_FOUND;
  }
}

EFI_STATUS
PoplarFastbootPlatformFlashPartitionEx (
  IN CHAR8  *PartitionName,
  IN UINTN   Offset,
  IN UINTN   Size,
  IN VOID   *Image
  )
{
  EFI_STATUS               Status;
  UINTN                    PartitionSize;
  FASTBOOT_PARTITION_LIST *Entry;
  CHAR16                   PartitionNameUnicode[60];
  BOOLEAN                  PartitionFound;
  UINTN                    BlockSize;
  EFI_DISK_IO_PROTOCOL    *DiskIo;

  AsciiStrToUnicodeStr (PartitionName, PartitionNameUnicode);
  PartitionFound = FALSE;
  Entry = (FASTBOOT_PARTITION_LIST *) GetFirstNode (&(mPartitionListHead));
  while (!IsNull (&mPartitionListHead, &Entry->Link)) {
    // Search the partition list for the partition named by PartitionName
    if (StrCmp (Entry->PartitionName, PartitionNameUnicode) == 0) {
      PartitionFound = TRUE;
      break;
    }

   Entry = (FASTBOOT_PARTITION_LIST *) GetNextNode (&mPartitionListHead, &(Entry)->Link);
  }
  if (!PartitionFound) {
    return EFI_NOT_FOUND;
  }

  // Check image will fit on device
  PartitionSize = (Entry->EndingLBA - Entry->StartingLBA + 1) * mFlashBlockIo->Media->BlockSize;
  if (PartitionSize < Size) {
    DEBUG ((DEBUG_ERROR, "Partition not big enough.\n"));
    DEBUG ((DEBUG_ERROR, "Partition Size:\t%ld\nImage Size:\t%ld\n", PartitionSize, Size));

    return EFI_VOLUME_FULL;
  }

  BlockSize = mFlashBlockIo->Media->BlockSize;
  Status = gBS->OpenProtocol (
                  mFlashHandle,
                  &gEfiDiskIoProtocolGuid,
                  (VOID **) &DiskIo,
                  gImageHandle,
                  NULL,
                  EFI_OPEN_PROTOCOL_GET_PROTOCOL
                  );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = DiskIo->WriteDisk (
                     DiskIo,
                     mFlashBlockIo->Media->MediaId,
                     Entry->StartingLBA * BlockSize + Offset,
                     Size,
                     Image
                     );
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "Failed to write %d bytes into 0x%x, Status:%r\n", Size, Entry->StartingLBA * BlockSize + Offset, Status));
    return Status;
  }
  return Status;
}

FASTBOOT_PLATFORM_PROTOCOL mPlatformProtocol = {
  PoplarFastbootPlatformInit,
  PoplarFastbootPlatformUnInit,
  PoplarFastbootPlatformFlashPartition,
  PoplarFastbootPlatformErasePartition,
  PoplarFastbootPlatformGetVar,
  PoplarFastbootPlatformOemCommand,
  PoplarFastbootPlatformFlashPartitionEx
};

EFI_STATUS
EFIAPI
PoplarFastbootPlatformEntryPoint (
  IN EFI_HANDLE                            ImageHandle,
  IN EFI_SYSTEM_TABLE                      *SystemTable
  )
{
  return gBS->InstallProtocolInterface (
                &ImageHandle,
                &gAndroidFastbootPlatformProtocolGuid,
                EFI_NATIVE_INTERFACE,
                &mPlatformProtocol
                );
}
