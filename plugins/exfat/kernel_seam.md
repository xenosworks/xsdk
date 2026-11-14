# The kernel's file-system seam (xboxkrnl 2.0.17559.0)

What a file-system driver implements so the kernel routes opens of `\Device\Mass<n>\...` to it, how a volume is
mounted on a device and taken off it, and where a module is loaded at boot. Evidence is my disassembly of
xboxkrnl 17559 (image base 0x80040000); every address below is in it, and the IDB carries the names used here
(`Iop*` = the I/O manager's internals, `Fat*`, `Hfs*`, `Disc*`, `Raw*` = the kernel's own file systems, named by
me). Export names and ordinals are Xenia's (xenia:src/xenia/kernel/xboxkrnl/xboxkrnl_table.inc). Nothing here has
run on a console yet; `tests/probe_exfat_seam.c` checks the parts a title can reach (section 8).

## 1. Objects

```c
/* IoCreateDevice (0x8006aea0): (DRIVER_OBJECT*, ExtensionSize, STRING* Name, DeviceType, Flags, DEVICE_OBJECT**).
   80 bytes, then the extension rounded up to 8. */
typedef struct DEVICE_OBJECT {
    USHORT          Type;                  /* +00 3 */
    USHORT          Size;                  /* +02 80 + extension */
    LONG            ReferenceCount;        /* +04 */
    DRIVER_OBJECT  *DriverObject;          /* +08 */
    DEVICE_OBJECT  *MountedOrSelfDevice;   /* +0C NULL on a mountable type (45 among them), self otherwise;
                                                  the mounted volume once a file system claims the device */
    ULONG           Unknown10;             /* +10 */
    ULONG           Flags;                 /* +14 0x10 not ready (set at create, cleared by the owner), 8 named,
                                                  1 never mount, 2 exclusive, 4 lock the caller's pages (passed
                                                  down to volumes), 0x20 dismount pending, 0x80 disc file system
                                                  allowed on other types */
    PVOID           DeviceExtension;       /* +18 this + 80, NULL without one */
    UCHAR           DeviceType;            /* +1C 45 = USB mass storage, 8 = file-system volume, 60 = SATA, 2 = disc */
    UCHAR           Unknown1D;
    UCHAR           StackSize;             /* +1E 1 at create; a volume adds its target's */
    UCHAR           DeletePending;         /* +1F */
    ULONG           SectorSize;            /* +20 a volume copies its target's */
    ULONG           AlignmentRequirement;  /* +24 a volume takes the larger of its own and its target's */
    KDEVICE_QUEUE   DeviceQueue;           /* +28 IoStartPacket's */
    KEVENT          MountLock;             /* +38 synchronization event, signaled at create; IopMountDevice holds it */
    ULONG           Unknown48[2];          /* +48 */
} DEVICE_OBJECT;                           /* 0x50 */

/* Constant, in .rdata; the kernel's own are FAT 0x80042f04, HFS 0x8004331c, raw 0x80043224, the USB
   mass-storage class 0x80044b10. IoCallDriver (0x8006ab48) indexes MajorFunction by the stack location's
   first byte; it has eleven entries: the three tables above end at the next string. */
typedef struct DRIVER_OBJECT {
    VOID     (*StartIo)(DEVICE_OBJECT*, IRP*);        /* +00 NULL unless IoStartPacket is used */
    VOID     (*DeleteDevice)(DEVICE_OBJECT*);         /* +04 may be NULL: the I/O manager then dereferences */
    VOID     (*DismountVolume)(DEVICE_OBJECT*);       /* +08 IoDismountVolume calls it; NULL = can't dismount */
    NTSTATUS (*MajorFunction[11])(DEVICE_OBJECT*, IRP*); /* +0C */
} DRIVER_OBJECT;                                      /* 0x38 */
```

Major function codes (confirmed by the kernel's own calls; 4, 5, 6 and 8 by their place and FAT's handlers):

| Code | Function                 | Who sends it                                                           |
|------|--------------------------|------------------------------------------------------------------------|
| 0    | CREATE                   | IopParseDevice 0x8006c0f0, `*(sp) = 0` at 0x8006c338                   |
| 1    | CLOSE                    | IopDeleteFile 0x8006c6e8 (the file object's delete procedure)          |
| 2    | READ                     | NtReadFile → 0x8006cb20 with 2                                         |
| 3    | WRITE                    | NtWriteFile → 0x8006cb20 with 3                                        |
| 4    | QUERY_INFORMATION        | FAT 0x800a2fa0                                                         |
| 5    | SET_INFORMATION          | FAT 0x800a3bf0                                                         |
| 6    | FLUSH_BUFFERS            | FAT passes it down (0x800a2750, shared with 9)                         |
| 7    | QUERY_VOLUME_INFORMATION | FAT's handler 0x800a2d18 answers classes 1, 3, 4, 5 with "FAT "        |
| 8    | DIRECTORY_CONTROL        | FAT 0x800a2a88                                                         |
| 9    | DEVICE_CONTROL           | IoBuildDeviceIoControlRequest 0x8006b770, `*(sp) = 9`; no FSCTL exists |
| 10   | CLEANUP                  | IopCloseFile 0x8006c5b8 on the last handle, `*(sp) = 10`               |

The kernel's tables (StartIo, DeleteDevice, DismountVolume, then 0..10):

```
FAT  0x80042f04: 0 800a25d0 800a2650 | 800a51c8 800a4d90 800a4890 800a4890 800a2fa0 800a3bf0 800a2750 800a2d18 800a2a88 800a2750 800a28a0
HFS  0x8004331c: 0 800b76e0 800b7738 | 800b8728 800b86a0 800b8390 8006b400 800b7eb0 800aff00 800b78a0 800b7cb0 800b7a30 800b78a0 800b7960
Raw  0x80043224: 0 0        800afa78 | 800b0008 800aff90 800afb98 800afb98 800afe90 800aff00 800afb98 800afd70 8006b400 800afb98 800afc40
Mass 0x80044b10: 800e34d0 0 0        | 8006b400 8006b400 800e1128 800e1128 8006b400 8006b400 8008f010 8006b400 8006b400 800e1868 8006b400
```

0x8006b400 is IoInvalidDeviceRequest: it completes the IRP with STATUS_INVALID_DEVICE_REQUEST. HFS is
read-only, so its WRITE is invalid. The Mass device queues READ, WRITE and most IOCTLs to its StartIo.

```c
/* IoAllocateIrp (0x8006b650) allocates the header and StackSize locations after it; CurrentStackLocation
   starts one past the last, and IoCallDriver moves it down 36 bytes before dispatching. */
typedef struct IRP {
    USHORT           Type;                 /* +00 6 */
    USHORT           Size;                 /* +02 */
    ULONG            Flags;                /* +04 low byte: 1 no cache (file opened without buffering), 8 the
                                                  caller waits on UserEvent and IoCompleteRequest copies IoStatus
                                                  to UserIosb, 0x10 no completion APC (the caller reads IoStatus),
                                                  0x20 unlock UserBuffer's pages, 0x40 unmap a segment array;
                                                  IopParseDevice sets 0x16, NtReadFile 0x10 or 0x11 */
    ULONG            Unknown08[2];         /* +08 the thread's IRP list (IoQueueThreadIrp) */
    NTSTATUS         IoStatus.Status;      /* +10 */
    ULONG            IoStatus.Information; /* +14 */
    CHAR             StackCount;           /* +18 */
    CHAR             CurrentLocation;      /* +19 */
    UCHAR            PendingReturned;      /* +1A */
    UCHAR            Cancel;               /* +1B */
    PVOID            UserBuffer;           /* +1C read/write data, IOCTL output buffer, or the mapped array */
    IO_STATUS_BLOCK *UserIosb;             /* +20 */
    KEVENT          *UserEvent;            /* +24 */
    union {                                /* +28 */
        LARGE_INTEGER AllocationSize;      /*     CREATE */
        struct { PVOID ApcRoutine; PVOID ApcContext; }; /* READ/WRITE */
    };
    KAPC             Apc;                  /* +30 completion in the requester's thread (0x8006e230) */
    ULONG            Unknown40;            /* +40 locked length with Flags 0x40 */
    KTHREAD         *Thread;               /* +44 */
    ULONG            Unknown48[2];
    IO_STACK_LOCATION *CurrentStackLocation; /* +50 */
    FILE_OBJECT     *OriginalFileObject;   /* +54 */
} IRP;                                     /* 0x58, then the stack locations */

typedef struct IO_STACK_LOCATION {         /* 36 bytes */
    UCHAR  MajorFunction;                  /* +00 */
    UCHAR  MinorFunction;                  /* +01 */
    UCHAR  Flags;                          /* +02 CREATE: the open packet's byte 32 */
    UCHAR  Control;                        /* +03 1 pending returned, 0x20 call completion on cancel, 0x40 on success,
                                                  0x80 on error; FAT sets 0xE0 on the location it passes down */
    union {                                /* +04 */
        struct {                           /*     CREATE (IopParseDevice 0x8006c2e4..0x8006c374) */
            ULONG   DesiredAccess;         /* +04 GENERIC_* already turned into specific rights */
            ULONG   Options;               /* +08 Disposition << 24 | CreateOptions & 0xFFFFFF */
            USHORT  FileAttributes;        /* +0C */
            USHORT  ShareAccess;           /* +0E */
            STRING *RemainingName;         /* +10 the path under the device, no leading backslash */
        } Create;
        struct {                           /*     READ, WRITE (0x8006cb20, IoBuildAsynchronousFsdRequest 0x8006b6d8) */
            ULONG         Length;          /* +04 */
            ULONG         Key;             /* +08 0; FAT refuses a read with another */
            LARGE_INTEGER ByteOffset;      /* +0C -1 = write at end of file; the data is IRP.UserBuffer */
        } Read;
        struct {                           /*     DEVICE_CONTROL (IoBuildDeviceIoControlRequest 0x8006b7b0..0x8006b7d0) */
            ULONG OutputBufferLength;      /* +04 output in IRP.UserBuffer */
            PVOID InputBuffer;             /* +08 */
            ULONG InputBufferLength;       /* +0C */
            ULONG IoControlCode;           /* +10 */
        } DeviceIoControl;
    };
    DEVICE_OBJECT *DeviceObject;           /* +14 */
    FILE_OBJECT   *FileObject;             /* +18 */
    NTSTATUS     (*CompletionRoutine)(DEVICE_OBJECT*, IRP*, PVOID); /* +1C STATUS_MORE_PROCESSING_REQUIRED stops */
    PVOID          Context;                /* +20 */
} IO_STACK_LOCATION;

/* ObCreateObject(&IoFileObjectType, ..., 104) in IopParseDevice (0x8006c384..0x8006c468). */
typedef struct FILE_OBJECT {
    USHORT         Type;                   /* +00 5 */
    UCHAR          Flags;                  /* +02 1 synchronous (CreateOptions & 0x30), 4 no buffering (& 8),
                                                  8 sequential (& 4), 0x40 random (& 0x800); set later: 0x10 closed,
                                                  0x20 cleaned up, 0x80 volume dismounted (FAT marks it) */
    UCHAR          Access;                 /* +03 0x20 read, 0x40 write (NtReadFile/NtWriteFile check them) */
    DEVICE_OBJECT *DeviceObject;           /* +04 the volume */
    PVOID          FsContext;              /* +08 the file system's (FAT: its FCB) */
    PVOID          FsContext2;             /* +0C the file system's (FAT frees it at dismount) */
    ULONG          Unknown10;              /* +10 */
    LARGE_INTEGER  CurrentByteOffset;      /* +14 read when the caller gives no offset or -2 */
    FILE_OBJECT   *RelatedFileObject;      /* +1C */
    PVOID          CompletionPort;         /* +20 non-NULL forbids an APC routine */
    ULONG          Unknown24;              /* +24 -1 at create */
    KEVENT         Lock;                   /* +28 synchronization event; serializes a synchronous file's I/O */
    KEVENT         Event;                  /* +38 notification event; the create waits on it when the FS pends */
    ULONG          Unknown48[2];           /* +48 */
    LIST_ENTRY     FsLink;                 /* +50 the file system's: FAT lists a volume's open files here */
    UCHAR          Unknown58;              /* +58 2 at create; IRP.Flags' top byte is this minus 2 */
    UCHAR          Unknown59[15];
} FILE_OBJECT;                             /* 0x68 */

/* The parse context NtCreateFile hands IopParseDevice (type 8, size 48). */
typedef struct OPEN_PACKET {
    USHORT         Type;                   /* +00 8 */
    USHORT         Size;                   /* +02 48 */
    NTSTATUS       FinalStatus;            /* +04 the parse writes the open's status here */
    ULONG          Unknown08;
    FILE_OBJECT   *RelatedFileObject;      /* +0C a relative open's; its device is the one parsed */
    LARGE_INTEGER  AllocationSize;         /* +10 */
    ULONG          CreateOptions;          /* +18 */
    USHORT         FileAttributes;         /* +1C */
    USHORT         ShareAccess;            /* +1E */
    UCHAR          Flags;                  /* +20 to the stack location's Flags */
    UCHAR          Unknown21[3];
    ULONG          Disposition;            /* +24 */
    ULONG          DesiredAccess;          /* +28 */
    ULONG          Unknown2C;
} OPEN_PACKET;

/* OBJECT_TYPE: Allocate, Free, Close, Delete, Parse, DefaultObject, PoolTag (28 bytes).
   IoDeviceObjectType 0x80040b08 (ordinal 0x3A): ExAllocatePoolTypeWithTag, ExFreePool, 0, 0, IopParseDevice 0x8006c0f0,
   0x801709d0, 'Devi'.  IoFileObjectType 0x80040b24: Close IopCloseFile 0x8006c5b8, Delete IopDeleteFile 0x8006c6e8,
   Parse 0x8006c580, DefaultObject 56 (the Event's offset in FILE_OBJECT), 'File'. */
```

## 2. The devices

IopCreateStorageDevices (0x800e1dd0, at boot) creates every removable-storage device object once, named, with
Flags 0x10 cleared and AlignmentRequirement 1, so they exist whether or not media is in: `\Device\Mu0`,
`\Device\Mu1` (and `\Device\Mu0System`, `\Device\Mu1System`), **`\Device\Mass0`, `\Device\Mass1`, `\Device\Mass2`**
(type 45, driver 0x80044b10, extension 0x1C: +0 the slot's static record, +4 and +12 QWORDs the partition's
offset and length, +20 the device), `\Device\Joul0`..`\Device\Joul3` (type 45 too, 0x800e2484),
`\Device\BuiltInMuUsb\Storage`, `\Device\TransferCable`, and symbolic links `\Device\UpdateStorage<n>`.
The USB mass-storage class driver attaches a stick to a slot later; the device object never changes.

The Mass driver answers (0x800e1868):

| IOCTL (NT's public value)              | Output                                                                   |
|----------------------------------------|--------------------------------------------------------------------------|
| 0x70000 IOCTL_DISK_GET_DRIVE_GEOMETRY  | 8 bytes: `ULONG Sectors` (the slot's +152), `ULONG BytesPerSector` (+156 holds the size minus one, a USHORT). 0x800e1208. Fails without media. |
| 0x74004 IOCTL_DISK_GET_PARTITION_INFO  | 16 bytes: `LARGE_INTEGER StartingOffset, PartitionLength`, the extension's +4 and +12 when the slot's flags (+52) have 0x800, else zeros. 0x800e1298. |
| 0x2404C IOCTL_CDROM_GET_DRIVE_GEOMETRY | what RawMountVolume asks a type-2 device instead of 0x70000             |
| others                                 | queued to StartIo 0x800e34d0 (verify and friends) or refused             |

The sector count is 32-bit, so a 512-byte-sector drive past 2 TB needs PartitionLength; whether the class driver
fills it for a stick is for the probe (section 8). FAT (FatMountVolume 0x8009f918) asks both, accepts a sector
size up to 0x1000, and reads the MBR at offset 0 (types 1, 4, 6, 0xB, 0xC, 0xE at 446, 0xAA55 at 510) or a bare
FAT boot sector; HFS (0x800b6978) accepts 512..4096.

A volume reads and writes its device with `IoSynchronousFsdRequest(IRP_MJ_READ or _WRITE, device, buffer, bytes,
&offset)` (0x8006b928, ordinal 0x4C): it builds the IRP above (Length +4, ByteOffset +0C, UserBuffer), queues it on
the thread, calls IoCallDriver and waits on its own event when the driver returns STATUS_PENDING (0x103). For
asynchronous I/O a volume copies the current stack location to the next one (28 bytes, FAT 0x800a27c4), sets a
completion routine (FAT's 0x800a7588) with Control 0xE0, and calls IoCallDriver(target, irp) itself. A device
with Flags 4 wants the caller's pages locked first (0x8006b440, as IoBuildAsynchronousFsdRequest does).

## 3. Mounting

Every open of `\Device\Mass0\<path>` reaches IopParseDevice (0x8006c0f0) with the device object. Without an open
packet, an empty remaining name and no Flags 0x10, it just references and returns the device object: that is how
`ObReferenceObjectByName("\Device\Mass0", 0, IoDeviceObjectType, 0, &device)` works. With a packet:

1. KeEnterCriticalRegion; IoAcquireDeviceObjectLock. DeletePending or Flags 0x10 → STATUS_NO_SUCH_DEVICE
   (0xC000000E). Flags 2 with a reference already and no related file → STATUS_ACCESS_DENIED.
2. While `MountedOrSelfDevice` is NULL: clear Flags 0x20, release the lock, **IopMountDevice(device,
   remainingName->Length == 0)** (0x8006c230), fail the open with its status if negative, re-lock.
3. Flags 0x20 (dismount pending) → 0xC0050004. Otherwise `volume->ReferenceCount++`, release the lock, build the
   CREATE IRP on the volume (section 1), create the FILE_OBJECT, IoCallDriver(volume, irp); STATUS_PENDING →
   wait on FileObject.Event; the status is the IRP's; the file object is the open's result.

IopMountDevice (0x8006bfa8) is the dispatcher: KeWaitForSingleObject(&device->MountLock) ... KeSetEvent at the end,
so a mount runs under the device's own lock. Already mounted → done. Flags 1 → STATUS_UNRECOGNIZED_VOLUME
(0xC000014F). Then by DeviceType:

```
60 (SATA)         sub_800952c8, then the FATX family
72                sub_80095730
2, 64 (disc)      DiscMountVolume 0x800ab6c0
45 (Mass, Joul)   FatMountVolume 0x8009f918      bl at 0x8006c048
                  → if 0xC000014F: HfsMountVolume 0x800b6978      bl at 0x8006c05c
                  → if 0xC000014F: DiscMountVolume 0x800ab6c0     bl at 0x8006c070   ← the seam
others            (Flags 0x80: DiscMountVolume) then sub_800a61e0 (FATX)
```

Then, if the status is negative and the open was of the device itself (empty remaining name), **RawMountVolume
0x800af850** attaches a raw volume (driver 0x80043224, type 8, extension 0x44): opens of the bare device never
fail for want of a file system, and XAM's queries go through it. A raw volume has a DismountVolume (0x800afa78) but
no DeleteDevice.

Each file system's mount does the same things (FatMountVolume, HfsMountVolume, RawMountVolume agree):
`IoSynchronousDeviceIoControlRequest(0x70000, device, 0, 0, &geometry, 8, 0)` and 0x74004 for the partition;
read and check the boot sector; `IoCreateDevice(&MyDriverObject, extensionBytes, NULL, 8, 0, &volume)`;
`volume->StackSize += device->StackSize`; `AlignmentRequirement = max`; `if (device->Flags & 4) volume->Flags |= 4`;
`volume->SectorSize = device->SectorSize`; `ObReferenceObject(device)`; the extension's first word = the device;
`volume->Flags &= ~0x10`; **`device->MountedOrSelfDevice = volume`** (a plain store at 0x800af95c, under the
mount lock the caller holds). "Not mine" is STATUS_UNRECOGNIZED_VOLUME, 0xC000014F, after IoDeleteDevice(volume)
if one was created; the dispatcher only continues the chain on that status.

## 4. What a mounted file system receives

- **CREATE**: the IRP in section 1 against the volume. The FS resolves `RemainingName` (ANSI, backslash-separated,
  may be empty for the root), fills `FsContext`/`FsContext2`, links the file object into its own list, sets
  `IoStatus.Status` and `Information` (FILE_OPENED and friends) and IoCompleteRequest(irp, 0), or returns
  STATUS_PENDING and completes later. The kernel never looks inside FsContext.
- **READ/WRITE** (NtReadFile and NtWriteFile → 0x8006cb20): the kernel checks the Access byte, takes
  `FileObject.Lock` for a synchronous file, resolves the offset (NULL or -2 → `CurrentByteOffset`; -1 is only a
  WRITE: append), refuses a no-buffering file's unaligned length or offset against `volume->SectorSize`
  (STATUS_INVALID_PARAMETER), sets IRP.Flags 0x10 (plus 1 without buffering), UserBuffer = the caller's buffer
  (locked when the volume has Flags 4; NtReadFileScatter maps a segment array instead and sets 0x40) and calls
  the volume. FAT's handler (0x800a4890) is the reference for the answers: a zero length completes with 0 bytes;
  a read at or past the file size → STATUS_END_OF_FILE (0xC0000011); a closed file (Flags 0x10) →
  STATUS_FILE_CLOSED (0xC0000128); a directory → STATUS_INVALID_DEVICE_REQUEST; a dismounted volume →
  STATUS_VOLUME_DISMOUNTED (0xC000026E); writes extend the file first. `Information` is the byte count.
  Who moves `CurrentByteOffset` afterwards is not pinned (section 8).
- **CLEANUP** (10) when the last handle closes, **CLOSE** (1) when the file object is deleted; both IRPs are
  synchronous (Flags 8, the kernel waits on its own event).
- **QUERY_INFORMATION / SET_INFORMATION / QUERY_VOLUME_INFORMATION / DIRECTORY_CONTROL**: NtQueryInformationFile
  0x8006d630, NtSetInformationFile 0x8006d928, NtQueryVolumeInformationFile 0x8006d490, NtQueryDirectoryFile
  0x8006cfa8 build them; their parameter layouts are step 3's to read when the driver implements each class.
  FAT answers volume classes 1 (FileFsVolumeInformation), 3 (size: 16 bytes + two counts), 4 (device: the target's
  DeviceType), 5 (attributes, name "FAT ").
- **FLUSH_BUFFERS and DEVICE_CONTROL**: FAT passes them to the device with a completion routine (0x800a2750).
- Completion: `irp->IoStatus.Status`, `irp->IoStatus.Information`, `IoCompleteRequest(irp, 0)` (0x8006ac20,
  ordinal 0x35; the second argument is the priority boost). IoInvalidDeviceRequest (0x8006b400, ordinal 0x41)
  is the slot for what the FS doesn't do.

## 5. Unmounting

`IoDismountVolume(device)` (0x8006b1d0, ordinal 0x3B): under the I/O spin lock 0x801a5618, takes
`device->MountedOrSelfDevice`; if its driver has a DismountVolume, adds a reference and calls it, then
IopDereferenceDevice (0x8006e150) drops the reference and deletes the volume once it reaches zero with
DeletePending set. `IoDismountVolumeByName` (0x8006b2f0) and `IoDismountVolumeByFileHandle` (0x8006b278) wrap it
for XAM; inside the kernel only a storage teardown loop (0x80149530: dismount, then IoDeleteDevice, per device)
calls it. The USB class driver has no reference to it, so what a pulled stick triggers is step 3's first question.

FatDismountVolume (0x800a2650) is the protocol a DismountVolume follows: KeEnterCriticalRegion; the volume's
own lock; IoAcquireDeviceObjectLock; already dismounted → STATUS_VOLUME_DISMOUNTED; else
**`if (device->MountedOrSelfDevice == volume) device->MountedOrSelfDevice = NULL`**, mark the volume dismounted;
IoReleaseDeviceObjectLock; mark every open file object (`Flags |= 0x80`) so later I/O fails with
STATUS_VOLUME_DISMOUNTED, and drop their FsContext2; release; **IoDeleteDevice(volume)** (0x8006b150, ordinal
0x39: sets DeletePending; deletes now if unreferenced, else when the last reference drops). FatDeleteDevice
(0x800a25d0) frees the extension's buffers, `ObDereferenceObject(device)`, `ObDereferenceObject(volume)`.

## 6. The seam, decided

**Production: a hook at 0x8006c070**, the `bl DiscMountVolume` in the type-45 chain (the twelve words from
0x8006c044 are `7fc3f378 480338d1 7c7f1b78 7f1fe800 409a005c 7fc3f378 4804a91d 7c7f1b78 7f1fe800 409a0048
7fc3f378 4803f651`; r30 is the device, r29 holds 0xC000014F). The stub in the .text padding (zeros from
0x801616c8 to 0x8016ffff, 0xE938 bytes) saves LR and the device, calls DiscMountVolume, and when it answers
0xC000014F asks `XexGetModuleHandle("exfat.xex")` and `XexGetProcedureAddress(handle, 1, &mount)` and returns
`mount(device)`; without the module it returns 0xC000014F as before, so a kernel with the stub but no module
behaves as today. No window exists in which the kernel answers "unrecognized" for an exFAT stick, and XAM's raw
volume never attaches to one.

**Development (step 3), and the fallback for a launch.ini load: the module polls.** A kernel thread wakes every
500 ms and, for each of the seven type-45 devices (section 2, referenced by name once), when
`MountedOrSelfDevice` is NULL or the raw volume (DriverObject == 0x80043224) and 0x70000 succeeds, reads sector 0
and, on exFAT's signature, takes `MountLock`, re-checks, calls `IoDismountVolume(device)` first if the raw volume
is there, and mounts. The same `mount(device)` serves both triggers. The kernel's chain and the poll share the
mount lock, and IopParseDevice re-reads `MountedOrSelfDevice` under the device object lock, so neither can mount
over the other. What the poll costs is the window, and what the kernel does in it is for the probe to record.

The module's DRIVER_OBJECT fills all eleven slots (IoInvalidDeviceRequest where it has nothing), DismountVolume
and DeleteDevice as FAT's; its volume is type 8 with the extension's first word the device, as every kernel file
system has it, so XAM's and the kernel's walks over a volume's extension find what they expect.

## 7. The boot

ExpSystemInitThread (0x800610e8) ends with `li r3, 0x79; bl 0x80061000` (boot marker 121), then at **0x80061388
`bl ExpCheckSystemUpdate`** (0x800674a8: waits for UsbdBootEnumerationDoneEvent 0x8017217c, opens
`\Device\Mass0\$systemupdate\systemupdate2pre.xex`, then the same under `\SystemRoot`), and when that fails,
`XexLoadImage("xam.xex", 0x40000008, 0x20449700, NULL)` at 0x800613b0 (the version is 2.0.17559.0 packed:
major << 28 | minor << 24 | build << 8 | qfe), KeBugCheck 74319 if it fails. AniStartBootAnimation (0x80061730,
ordinal 678) loads `\Device\Flash\bootanim.xex` with 0x40000009: the kernel names a flash file by its full
`\Device\Flash\` path, and bit 0 marks an executable, so a DLL takes xam's 0x40000008. 0x40000000's meaning is
not pinned; both kernel loads carry it, so the stub does too.

The load stub replaces the `bl` at 0x80061388 with a `bl` into the padding: save LR in a frame,
`XexLoadImage("\Device\Flash\exfat.xex", 0x40000008, 0x20449700, NULL)`, restore, `b ExpCheckSystemUpdate`, so the
update check's status still returns to its caller. r31 holds 0x80170000 there and XexLoadImage keeps it, as every
kernel call keeps r14-r31. The module imports xboxkrnl only (xam.xex is not loaded yet) and must find the slots
empty: Usbd is up but no stick need be in. The words at 0x80061380 are `38600079 4bfffc7d 48006121 7c7e1b79`.
xeBuild carries both stubs and the file in its flash file-system list; its patch format is the user's ground.

## 8. Unknowns, and what the probe records

- Answered on the console (2026-11-14, results/2026-11-14-milestone-77-step-1, an exFAT stick): opening
  `\Device\Mass0\` fails with C000014F (unrecognized volume) and attaches nothing; opening `\Device\Mass0` itself
  succeeds and attaches the raw volume (driver 0x80043224), which stays after the handle closes (refcount 0) and
  makes a later `\Device\Mass0\` open fail with C000003A (object path not found). So the poll dismounts a raw
  volume before mounting, and the hook sits before the raw fallback.
- Answered: 0x74004 answers start 0, length 0xE53800000 for a 58,680 MB stick, a 64-bit length for the whole
  device. The class driver reads no partition table; a file system reads the MBR itself, as ours will for the
  first type-07 partition.
- Who advances `CurrentByteOffset` after a synchronous read: the FS or the kernel's completion. Step 3 tests two
  reads on the driver.
- What a pulled stick does to a mounted volume with no in-kernel caller of IoDismountVolume: step 3 pulls one.
- Whether kernel .text is writable at run time (only if a run-time patch is ever wanted): the probe logs
  MmQueryAddressProtect for the padding, the mount chain, .rdata and .data.
- NtReadFileScatter's UserBuffer (a mapped segment array): step 3 checks whether it is one flat mapping.
- The kernel's file cache (Fsc; FAT reads through 0x8006f0d8/0x8006f2b0): the driver starts without it.
- The storage power notification (KeRegisterDriverNotification 0x80070fa8, list 0x80170650, called from the
  power-down path 0x80071170) is not device arrival; nothing in the kernel tells a driver a stick arrived except
  the open that follows. Hence the hook or the poll.

## 9. What step 3 answered (2026-11-14, results/2026-11-14-milestone-77-step-3)

The module's layouts are plugins/exfat/kernel_io.h, each with a static assert and its address; FAT's answers that the
module mirrors are cited at their uses in plugins/exfat/exfat_module.c. Found on the console:

- The kernel keeps 64-bit fields 4-byte aligned (IRP read/write `ByteOffset` at +0x0C of the stack location,
  FILE_OBJECT `CurrentByteOffset` at +0x14): `KERNEL_UINT64` is packed and 4-aligned, read by value.
- The file system advances `CurrentByteOffset` for synchronous file objects, not the kernel's completion: two
  offset-less NtReadFile calls read 0-99 then 100-199 only when the module advances it (exfat_check).
- The ordinals in sections 4 and 5 were wrong in the first draft and are corrected: IoCompleteRequest 0x35,
  IoDeleteDevice 0x39, IoDismountVolume 0x3B, IoInvalidDeviceRequest 0x41 (imports/imports.txt, Xenia's table).
- The dismount protocol of section 5 holds: IoAcquireDeviceObjectLock (0x2E, no arguments), MountedOrSelfDevice
  cleared, IoReleaseDeviceObjectLock (0x44), file objects marked 0x80, IoDeleteDevice; the module's DeleteDevice
  runs when the last reference drops, and a loaded module waits for it before it unloads.
- NtWriteFile (0x8006cb20) enters a critical region and locks the buffer's pages (flag 0x20, for a device with
  flag 4) before calling the file system; neither costs a device IRP issued from the dispatch anything (speed11,
  speed12). What does: another thread waiting on the module's lock while a stream holds it slows the stream's
  later device IRPs (9 MiB/s against 15), so the module's poll takes the lock with RtlTryEnterCriticalSection
  (0x141) and skips a busy module (speed13 to speed15). The mechanism in the kernel's scheduler is not known.
- A pulled stick while mounted: left to the user (the module dismounts at the next poll whose geometry IOCTL
  fails).
