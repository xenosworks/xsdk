# Milestone 78: exFAT on the internal drive

Agreed on 2026-11-15, after milestone 77's module loads from the NAND and mounts a stick at boot
(results/2026-11-14-milestone-77-step-4). The user wants the internal drive's content partition formatted exFAT, for
files over 4 GB on the drive the console reads fastest, with the format chosen from a title that has a UI. The user's
drive is swapped for a spare before any format or raw write to it; they are told before the first one.

## Decisions (the user, 2026-11-15)

1. **The content partition alone.** `\Device\Harddisk0\Partition1`, where Aurora, the content and xsdk's files live,
   becomes exFAT; the kernel's other partitions on the drive stay FATX, since the kernel and xam use them directly.
   The kernel names them in its image (0x80042e1c-0x80042ec8): Partition0 (the whole drive), Partition1,
   SystemPartition, SystemAuxPartition, SystemExtPartition, SystemURLCachePartition, TitleURLCachePartition,
   WindowsPartition and DumpPartition. The dash's own Format writes the FATX layout to all of them and stays the way
   to prepare a fresh drive and the way back.
2. **The format comes from our formatter title,** milestone 77's step 5 (`exfat_format.xex`, docs/scope/77-exfat.md)
   with the drive's content partition in its list. No window in the dash: System Settings is a scene inside
   Microsoft's dash.xex, and a dialog there means patching that binary and its XUI resources.
3. **No serial-number prompt.** The dash asks for it; our title doesn't, by construction.

## Steps

1. **The content partition in the module** (plugins/exfat/exfat_module.c). The module polls
   `\Device\Harddisk0\Partition1` beside `\Device\Mass0` to `Mass2`, takes the partition device's type (read on the
   console first; the sticks' is `DEVICE_TYPE_MASS_STORAGE`), and mounts the partition as it mounts a stick when its
   first sector is exFAT; on a FATX drive it does nothing, so the module stays as it is for everyone who doesn't
   format. Two things differ from a stick:
   - **Boot order.** xam's storage thread dismounts and remounts every storage device present at boot and the dash
     waits for that pass (results/2026-11-14-milestone-77-step-4/README.md, "xam's storage start-up"). DashLaunch
     then reads launch.ini and loads its plugins from the drive inside the first executable's load, a second after
     the pass, before our gate (`XexExecutableModuleHandle`) opens ("DashLaunch's start-up"). So the content
     partition must be ours before DashLaunch reads it: the module mounts it as soon as the kernel's own mount of it
     has failed and xam's pass has let go of it, and remounts it whenever that pass dismounts ours. The step's first
     build only records that timeline from inside a boot with an exFAT partition (when xam dismounts, when the kernel
     remounts raw, when DashLaunch's first read comes), and the mount point is chosen from it, not guessed; the gate
     that holds the log and the stick poll stays where it is.
   - **The log.** It lives on the partition, so it opens after the mount, and the "loaded" line moves after it.
   Check, on a console whose content partition the title formatted exFAT (step 2): the console boots to Aurora from
   the drive with DashLaunch's drive plugins loaded (the console agent answers, Xbdm's and Engine's modules are
   loaded); exfat.log holds the mount from the boot; exfat_check's checks pass with their paths on the drive; a
   profile on the drive signs in, a save writes and reads back; the dash's storage screen shows the drive with its
   size and free space; a stick in USB0 mounts beside it; the dash's Format returns the drive to FATX and the console
   boots as before. The boot's time to the dash is logged against the FATX boot (the user times it on the screen).
2. **The formatter title** (`exfat_format.xex`, milestone 77 step 5's design, built once for both): the list shows
   the Mass devices and the drive's content partition, each with its size and its first sector's file system; the pad
   picks one; A held for three seconds confirms; the drive asks once more on its own screen, saying that everything on
   the content partition goes and the system partitions stay; then the dismount, step 2's formatter through write
   IRPs, the read-back and the report. The title holds the device's mount lock through the format, as the kernel
   holds it to mount (plugins/exfat/kernel_seam.md, section 3), so neither the kernel nor the module mounts what is
   half written; the module reads sector 0 under that lock for it, and tries a device nothing has mounted at every
   poll, so a dismounted stick mounts once the format has let go. `exfat_format.txt` names a device for
   console-agent runs as before. After the
   drive's format the console has no dash on it: DashLaunch reads launch.ini from a USB stick first, so a stick
   holding Aurora and a launch.ini brings the dash back, and Aurora copies itself and the plugins onto the drive. The
   title says so on its last screen.
   Check: a stick and the drive each format from the title and pass step 1's checks; a device name in the file that
   holds no media is refused; the title with neither pad nor file only lists; the drive's extra confirmation
   refuses a single A. The stick's part passed on 2026-11-15 (results/2026-11-15-milestone-78-step-2: the title's
   format, PC chkdsk clean, the module's mount, exfat_check 157 of 157); the drive's waits for the spare drive.

Order: step 2's title is written first, since step 1 cannot be checked before the drive is exFAT, and both are checked
on the drive in one session, with a spare drive or the user's own copy of the content partition, as the format erases
it.

## Open

- Whether the module can mount the partition in time for DashLaunch's reads without a code patch: step 1's timeline
  answers it. If it cannot, the fallback is DashLaunch's plugins and launch.ini on a stick or in the flash, which the
  user already has for launch.ini, and that is said before the step is called done.
- Whether xam keeps the drive's "unformatted" state from its boot pass after our mount: the storage screen check
  shows it. If it does, the dash's view of the drive is wrong while Aurora's and titles' are right, and the fix is
  xam's notification path, a step of its own.
- Drives over 2 TB: the content partition's sector numbers pass 32 bits there. The core's offsets are 64-bit; the
  kernel's SATA path past 2 TB is milestone 77's open question still. Not promised.

## Left out

A dash window and the serial-number patch (decided above); reading the drive on a PC, which needs a partition at the
console's offset (Linux mounts it with a loop offset, Windows doesn't); the other partitions; a restore of Aurora
from a stick by the title; memory units and the disc drive.
