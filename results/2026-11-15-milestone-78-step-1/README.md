# Milestone 78 step 1: the content partition in the module, the boot timeline

docs/scope/78-exfat-hdd.md, step 1. The module (plugins/exfat/exfat_module.c) takes `\Device\Harddisk0\Partition1`
as a fourth device, of device type 7 (DEVICE_TYPE_DISK, read on the console in step 2's listing run), mounted like a
stick when its first sector is exFAT and left alone while the kernel's FATX volume is on it. Its log lives on that
partition, so the "loaded" line, the mounted volumes' lines and the boot timeline wait for the log's folder, which the
thread tries at every poll; a freshly formatted partition gets `XSDK` and `XSDK\exfat` made.

The boot timeline: the thread records each change of what the partition's device holds: no device, no volume, the
kernel's raw volume, another file system's volume (FATX), our volume; the gate's opening is recorded too, all timed
from the module's start. The step's mount point for an exFAT partition is to be chosen from that record (the scope's
step 1), not guessed.

## Run 1: sampling before the gate, a FATX drive

The module `exfat_timeline_run1.xex` (md5 9f91a856...) at hdd:\XSDK\exfat\exfat.xex, booted through the shadowboot
loop's HDD-stub ROM (results/2026-11-14-milestone-77-step-4, run 29's ROM, md5 d562b81a...), the user's exFAT stick in
USB0, the drive FATX as the user runs it. This build sampled from the module's start, before the gate on
XexExecutableModuleHandle: one ObReferenceObjectByName of the partition's device, then reads of its device object
every 10 ms, no IRP and no open. Purpose: the boot passes with the fourth device and the sampling before the gate,
the module leaves the FATX partition alone, the stick still mounts, and the timeline of a FATX boot is the baseline
for the exFAT boot on the spare drive.

**Hung.** The console never answered again (console_info timed out for two minutes) and the user's photo shows the
shadow ROM's progress bands (green, yellow, magenta, white and blue rows: the SB's and CD's painters, run 8 onwards
of the step 4 record), which the boot animation paints over on a boot that gets that far: the hang is before xam
starts the animation, inside the window where the module's only new work is the sampling before the gate. Run 29
booted the same ROM with the production module, whose thread sleeps through that window. The console needed a power
cycle.

## Run 2: the same module with the sampling held until the gate

One change from run 1: the thread sleeps through the gate as the production module does, and the timeline starts at
the gate's opening (`exfat_timeline_run2.xex`, md5 a54c9dbc...). Purpose: whether run 1's hang was the sampling
before the gate; the rest of run 1's purpose.

**Hung the same way**: no answer, the user's photo the same band frame. So the sampling before the gate was not the
cause, or not alone. Earlier shadow hangs with a module running (step 4's runs 20 to 27) froze the boot animation;
the band frame means neither run reached xam's start of it. Both images have the same XEX layout as run 29's module
(xextool: load address 0x91580000, image size 0x31000, the same 34 pages; only the entry point moved), so the loader
sees the same shape. What differs from run 29 besides the module's code: the NAND now carries the exFAT image, not
the plain one (the shadow ROM's patch set is the RAM image's, proven in run 29 by the HDD stub's load; what the NAND's
own exfat.xex in the flash file system does in a shadow boot was never a question before); the stick in USB0 is the
title's format, not the PC's; ExShadowBoot was launched by the console agent, not from Aurora; and the module file
was put on the HDD a minute before each launch, so a write not yet on the disk when the payload clears memory is
possible too.

## Run 3: the control, run 29's module

`exfat_control_run3.xex` (md5 5736a967..., the module run 29 booted with and the NAND carries) at
hdd:\XSDK\exfat\exfat.xex, the same ROM, launched the same way. Before it, the run 2 file on the HDD is fetched and
compared with its md5, for the write-cache question. Purpose: whether the shadow path still boots at all since the
NAND was reflashed; if it does, the cause is in the module's code since run 29 (commit 98a9185 or step 1's changes),
and the next run boots 98a9185's module alone.

The run 2 file fetched back from the HDD after the power cycle has run 2's md5 (a54c9dbc...), so the write cache is
not it. **The control hung too**: no answer in two and a half minutes, the user's photo the same frame. Run 29's own
module no longer boots through this ROM, so the module's code since run 29 is not the cause; the environment is.
What changed since run 29, by what it costs to undo: the module resident in the kernel when ExShadowBoot runs (none
in run 29; the NAND's in runs 2 and 3, an HDD-loaded one in run 1), undone by exfat_unload.txt; the stick in USB0,
the title's format since step 2, undone by pulling it; the launch by the console agent instead of from Aurora;
the NAND, the exFAT image instead of the plain one, undone only by a flash.

The user then pointed at the cause: the shadow ROM was cut from the 22:32 image, and the NAND has carried the 13:16
image since the flash, whose bootloaders were generated afresh (new nonces), so the ROM in the loop no longer
matched the console's state. `make_rom.py` rebuilt `out\hdd\xboxromw2d.bin` from the flashed `updflash.bin` with the
same HDD-stub records (78 records, the 2 exFAT ones cut, 2 added; md5 2dc5ab5a..., 1.14 MB differ from run 29's ROM);
the ROM carries the console's fuse data in the patch set's header, so it stays local, as before.

## Run 4: the control module through the rebuilt ROM

The control module (5736a967...) through the rebuilt ROM, launched by the agent, the stick out, the NAND's module
resident. Purpose: whether the rebuilt ROM boots; the stick and the resident module are the other two variables
still removed or present.

**Hung**, and the run was void: the rebuilt ROM was `make_rom.py`'s output alone, never passed through
`shadow_patch.py`, so its SB still ran the shadowboot path with the RAM pointer that the retail CD takes as a NAND
offset (step 4, run 7), and it carried no painters. The user did not photograph this run; its screen is unrecorded.
The console needed a power cycle.

The three earlier hangs are explained by the photos alone: green row 0 and white rows 10 to 12 and 15 to 21 are the
SB's codes, yellow rows 1 and 3 to 9 the CD's 0x41 and 0x43 to 0x49, **magenta row 2 the CD's panic 0xB3**, the CE's
SHA-1 against the hash the CD carries, painted after 0x49: step 4's run 10 frame exactly. Runs 1 to 3 used run 29's
ROM, cut from the NAND dump of 2026-11-14 (flashdmp.bin), while the NAND has held the 13:16 image since the user's
flash; the CD in the ROM is the dump's, the CE it reads from the NAND is the 13:16 build's, encrypted down another
key chain. The module's code, the stick and the agent's launch had nothing to do with it.

The ROM pipeline, confirmed by reproducing run 29's ROM (md5 d562b81a...) from the dump: `make_rom.py <image>
<rom> --addon out/exfat_hdd.bin`, then `shadow_patch.py <rom> <rom out> --rom-base 0x1601000 --painter
out/sb_painter.bin --cd-painter out/cd_painter.bin --cd-probe out/cd_probe.bin` (the script's self-check demands the
probe; runs 20 to 29 ran with it). The 13:16 image's ROM made that way is md5 553f5b34... (unpatched 2dc5ab5a...,
run 4's), installed as `out\hdd\xboxromw2d.bin`.

## Run 5: the control module through the 13:16 image's ROM

The control module (5736a967...) at hdd:\XSDK\exfat\exfat.xex, the 553f5b34... ROM at hdd:\XSDK\shadow\hdd, the
stick out, launched by the agent. Purpose: the loop boots again, as run 29 did, with the ROM matching the NAND.

**Booted to Aurora**: the agent answered about 100 s after the launch with an uptime of 116 s (the earlier boot's was
663 s), so the console rebooted at the launch and came back; exfat.log gained one "loaded" line (`exfat_run5.log`,
the HDD stub's module is the control, the same build the NAND carries, so the line alone cannot tell the two
apart). The loop works again; the module's code was never the cause.

## Run 6: the step 1 module, sampling from its start, a FATX drive

The module rebuilt with run 1's design, the timeline sampled before the gate as well (`exfat_timeline_run6.xex`, md5
8657074e...; plugin_main's comment corrected), at hdd:\XSDK\exfat\exfat.xex, the same ROM, the stick out, the drive
FATX. Purpose: run 1's purpose, with the stick's part left out: the boot passes with the fourth device and the sampling
before the gate, the module leaves the FATX partition alone, and the FATX boot's timeline is the baseline.

**Booted to Aurora** (uptime 85 s when the agent answered), and the log (`exfat_run6.log`, its last four lines) says
"4 volumes": the step 1 module, so the shadow ROM's stub loaded it from the HDD. The timeline:

    boot: +0 ms, \Device\Harddisk0\Partition1: another file system's volume
    boot: +1599 ms, the executable set, the gate open
    boot: +1600 ms, \Device\Harddisk0\Partition1: another file system's volume

So on a FATX drive the kernel's volume is on the content partition before the module's thread first runs, and the
gate opens 1.6 s later; the partition's state never changed through xam's storage pass or after it, and the module
mounted nothing (no Partition1 line). Sampling before the gate, a name lookup and device-object reads, passes the boot:
run 1's attribution to it was wrong. The repeated state line after the gate is the recorder's: it dedupes consecutive
samples only, and the gate's line sits between them.

What the FATX baseline settles for step 1: the mount point cannot be "when the kernel's volume appears", since it is
there already; the exFAT drive's timeline (raw volume or none at +0 ms, what xam's pass does to it, when the gate
opens) decides between mounting at the module's start and mounting at the gate. That run needs the spare drive formatted
by the title, with the module reachable while the drive holds no FATX: a stub variant loading it from a stick
(`\Device\Mass0\XSDK\exfat\exfat.xex`, the same 124-byte add-on with another path) or the NAND.

## The spare drive: the user's format and the first exFAT boot

The user swapped the spare drive in (the console reboots on a swap) and flashed a NAND carrying the run 6 module
(`xeBuild\17559\exfat.xex` = `exfat_timeline_run6.xex`, the same `-a exfat -a hvFixKeys` build as the 13:16 image;
its updflash.bin is the one on the stick below, md5 4d9cc15e...). The shadow loop is out for an exFAT drive: its stub
loads the module from the partition the module itself would mount. So these boots run the NAND's module.

The console agent came from a FAT32 stick in USB0 (`launch.ini`: NXE as the default, `plugin1 = Usb:\devagent.xex`;
`devagent.xex`, `devagent.ini`; `XSDK\exfat_format.xex`, `XSDK\exfat_check.xex`), the NXE dash from the flash.
`exfat_format.xex` launched from the stick by the agent; the user formatted `\Device\Harddisk0\Partition1` through
the pad (pick, A held 3 s, the drive's confirmation screen, A held 3 s again): the pad check of step 2. The title's log
(`exfat_format_drive.log`, on the stick as `game:\exfat_format.log`) shows the partition **held FATX**, not nothing:
the spare drive had a FATX format the dash called unformatted. The log ends at "formatting exFAT, label """: the
title closes its log before the drive's dismount because the log would live on the drive, but here it lived on the
stick, so the dismount status, the format's result and whether the module mounted within 1.5 s are unrecorded
(the user: "formatted fine"; fix: close the log only when the title runs from the drive). The UI formats with an
empty label.

The console then rebooted (the agent's uptime was 240 s before the format and 397 s long after it) and the module's
log on the fresh volume, `exfat_drive_boot.log`, holds one boot:

    exfat: loaded, kernel 2.0.17559.0, image 0x91580000-0x915B1000, 269 KB a volume, 4 volumes
    boot: +1 ms, \Device\Harddisk0\Partition1: no volume
    boot: +3188 ms, the executable set, the gate open
    boot: +3204 ms, \Device\Harddisk0\Partition1: no volume
    boot: +4606 ms, \Device\Harddisk0\Partition1: our volume
    \Device\Harddisk0\Partition1: mounted exFAT "" serial F46F5E10: 302585856 sectors of 512 from sector 2048, 128 KB clusters, 1181921 of 1181928 free, volume device 3A129958

**The exFAT boot timeline**: the partition's device exists when the module starts and holds no volume at all (not
the kernel's raw volume) from then through xam's storage pass to the gate, which opens 1.6 s later than on the FATX
drive (3.2 s against 1.6 s); the module's first poll after the gate mounts it, the mount taking 1.4 s on the 155 GB
volume. The dash's storage settings call the drive unformatted (the user), as xam registered it during its pass, when
nothing was on it; XeXMenu lists it. Why the post-format boot left no log lines (the module should have mounted the
fresh volume within a poll of the title's release and logged) is open: the title's log was cut, and the user's
reboot came before or after; the log writer appends per line (src/plugin.c), so nothing was overwritten.

What the two timelines say about the mount point: a volume mounted at the gate serves titles launched afterwards, but
neither xam's device registration (storage settings, profiles, the content partition) nor DashLaunch's launch.ini
and default dash, which precede the gate. To serve those the module has to mount at its start, +1 ms, with the device
present and empty, and survive xam's pass (which dismounts and remounts what it registers, step 4), or register as a
file system driver so the kernel's own remount after the pass lands on it. Either is a design step; the shadow loop
can host it only with a stub reading the module from a stick (`\Device\Mass0\...`), whose availability at the hook
is untested, else each attempt costs a flash.

## Run 7: the shadow loop with the module on the stick

The user's reboot after the format was deliberate, which settles the missing lines: on the format's boot the drive
held FATX, so the module had made `XSDK\exfat` on the kernel's FATX volume and set its log flag; the format took the
folder, the module re-mounted the fresh volume (XeXMenu listed it), and every later line failed on the missing
folder, silently (the writer opens per line and gives up quietly). Fix in the module: a mount of the partition's
volume clears the log flag, so the thread remakes the folder and logs the loaded line, the timeline and the mounted
volumes again (`exfat_timeline_run7.xex`, md5 5d84bf41...).

For the loop on an exFAT drive, `local/shadowboot/kernel_patch_usb.S`: the production stub with the module at
`\Device\Mass0\XSDK\exfat\exfat.xex` (built against the kernel image with tools/exfat_patch.py into
`out\exfat_usb.bin`, 108 bytes, records 0x163000 of 22 words and 0x613dc). The ROM `out\usb\xboxromw2d.bin` (md5
62d3e000...) comes from the flashed 15:57 image with that add-on, patched as the others; ExShadowBoot.xex and
HvxShadowBoot.bin are the HDD variant's. On the stick: `XSDK\shadow\` (the three files) and `XSDK\exfat\exfat.xex`
(the run 7 module); launched from the stick by the agent, the drive exFAT, the NXE dash from the flash.

Purpose: whether `\Device\Mass0` serves a file at the hook (a boot with "loaded ... 4 volumes" lines appended to the
drive's exfat.log means the stick's module ran, since the ROM's patch set has no flash-path stub); a boot without
them means the stick is not up that early and every module attempt on an exFAT drive costs a flash. The log's
re-made folder is checked by the same lines.

**Hung, black screen** (the user), no answer in 150 s. Not the band frame: either the chain died before the SB's
first band, or the bands went into a frame the NXE dash does not show (ExShadowBoot takes the frame address from
the display registers of the title that launched it, Aurora in every earlier run, the NXE dash here), with the hang
anywhere after. The ROM matches the NAND this time: the 15:57 updflash.bin is the newest image on the PC and
byte-identical to the stick's copy XeLL flashed (md5 4d9cc15e...).

The stick's `exshadow_run7.log` settles it: "Loaded HvxShadowBoot.bin ... size 0x65c, HvxShadowBoot = 0xc,
HvxQuiesceProcessor = 0x2f0", no "Display" or "Frame" line, and the log ends at "USB power down". Runs 5 and 6 ran
a payload of 0x7c0 bytes (HvxShadowBoot = 0x10) and a title that logged the frame address: the pair the HDD held.
The stick got `out\hdd\ExShadowBoot.xex` and `out\hdd\HvxShadowBoot.bin`, stale copies from 2026-11-14 23:27 and
23:33 that the HDD's files had long outgrown (the current pair is `out\ExShadowBoot.xex` of 00:34 and
`out\HvxShadowBoot.bin` of 02:59). So run 7 died in the old title or payload, before the ROM ran, and says nothing
about the stub's load from the stick; the ROM is untested. The drive's exfat.log has no run 7 lines, as expected, and
the power cycle's boot repeated the exFAT timeline (+0 ms no volume, gate +3204 ms, our volume +4622 ms).

## Run 8: run 7 with the current title and payload

`out\ExShadowBoot.xex` (md5 98d820b9...) and `out\HvxShadowBoot.bin` (md5 27e9dd2a..., 0x7c0 bytes) replace the
stale pair on the stick (and in `out\hdd` and `out\usb`); the ROM (62d3e000...) and the module (5d84bf41...) as in
run 7. Launched from the stick by the agent, from the NXE dash. Purpose: run 7's.

**Booted** (uptime 96 s when the agent answered, from the stick again). `exshadow_run8.log` has the frame line and
ends at "USB power down", the last line the title can write before it powers the stick it logs to down. The drive's
exfat.log (`exfat_drive_after_run8.log`) gained a boot block, which only the stick's module can have written, since
the ROM's patch set holds the stick-path stub and nothing else loads a module in a shadow boot:

    exfat: loaded, kernel 2.0.17559.0, image 0x91580000-0x915B1000, 269 KB a volume, 4 volumes
    boot: +0 ms, \Device\Harddisk0\Partition1: no volume
    boot: +2387 ms, the executable set, the gate open
    boot: +2403 ms, \Device\Harddisk0\Partition1: no volume
    boot: +3838 ms, \Device\Harddisk0\Partition1: our volume
    \Device\Harddisk0\Partition1: mounted exFAT "" serial F46F5E10: ...

So `\Device\Mass0` serves a file at the hook, and the loop hosts module attempts on an exFAT drive from the stick
(`usb0:\XSDK\exfat\exfat.xex`, `usb0:\XSDK\shadow\ExShadowBoot.xex`); a hang costs a power cycle, the NAND's module
boots the console again. The exFAT timeline held its shape in the shadow boot, with the gate 0.8 s earlier (2.4 s).
The log's remade folder is not exercised here (no format since), only not broken: the lines appear as before.

## Run 9: one mount attempt at the module's start

The user chose the mount-at-start experiment. `exfat_mount_at_start_run9.xex` (md5 dfefce90...): the volumes are set
up before the gate; while the thread waits for the executable it samples as before and, once, when the partition's
device is there and holds nothing, polls that one volume (media check, sector 0, the mount); nothing else happens
before the gate, so a dismount by xam's pass is only recorded, and the main loop re-mounts after the gate as it does
today. Sticks keep the gate. On a FATX drive the kernel's volume is already there, so the attempt never happens.
Through the loop from the stick, the same ROM, the drive exFAT, the stick's launch.ini (NXE default).

Purpose: whether the boot survives a volume of ours on the content partition through xam's storage pass, and what
the pass does to it: left alone (then the dash's storage settings should show the drive, and Aurora on the drive as
DashLaunch's default is the next run), dismounted and left empty, or replaced. A hang leaves no timeline.

**Booted** (uptime 96 s, the NXE dash, the agent from the stick), and the timeline (`exfat_drive_after_run9.log`,
its last block):

    exfat: loaded, kernel 2.0.17559.0, image 0x91580000-0x915B1000, 269 KB a volume, 4 volumes
    boot: +1 ms, \Device\Harddisk0\Partition1: no volume
    boot: +1653 ms, \Device\Harddisk0\Partition1: our volume
    boot: +2153 ms, the executable set, the gate open
    boot: +2153 ms, \Device\Harddisk0\Partition1: our volume
    \Device\Harddisk0\Partition1: mounted exFAT "" serial F46F5E10: ... volume device 3A129ED8

The mount at the module's start took 1.65 s and the volume was still there at the gate, 0.5 s later, with no sample
of the device empty or holding anything else in between: xam's storage pass left our volume alone, as far as samples
every 500 ms see, and the boot came through. The volume device at the gate is the one logged after it (3A129ED8), so
no re-mount happened before the log opened.

The dash's storage settings **still call the drive unformatted** (the user). That is not the file system's doing:
before the user's format the drive held FATX and the kernel's own volume was on the partition from the start (the
module made its log folder on it), and the dash called it unformatted then too. What the two states share is the
drive, so xam's verdict rests on something of the drive's, the security sector at byte offset 0x2000 ("Josh", the
drive's serial, firmware and size, RSA-signed) being the known one: a drive without a valid one shows as unformatted
whatever its partition holds, and the dash's Format writes it. The title's format writes Partition1 only, from byte
0x130EB0000, so it neither makes nor breaks that sector. To settle it: the user's knowledge of the spare drive, or a
probe reading sector 16 of `\Device\Harddisk0\Partition0`; if the sector is missing, the dash's Format (FATX plus the
sector), then the title's exFAT format again, then the storage screen again.

The user did not know, so `tests/probe_hdd_security.c` (a probe title, `probe_hdd_security.xex` on the stick) read
4 KB at byte 0x2000 of `\Device\Harddisk0\Partition0` through a read IRP: the whole disk has 312581808 sectors of 512
(160 GB) and **the sector is all zero**, no "Josh" magic, no fields, no signature (`probe_hdd_security.log`). The
user then placed the drive: it is their original drive, wiped on purpose (hence FATX without the sector), and the
dash's Format would make it show. So the drive has no security sector, and xam's verdict has nothing to do with what Partition1 holds; our volume
in place before xam's pass was never going to change it. The user declined the dash's Format (it asks for the
console's serial, and a FATX drive with a sector showing up is a tautology), so the sector question is parked: it
decides only xam's listing, profiles and saves on the drive, and needs a drive with a valid sector (a hddss.bin
written back at 0x2000, or a drive that has one) before a shadow boot with the run 9 module and a look at the storage
screen. Aurora, plugins and launch.ini from the drive go through the kernel's volume and need no sector; they are
next.

## Run 10: DashLaunch's default from the exFAT drive

No Aurora install is on the PC, so the stick's XeXMenu stands in: its 30466048-byte package
`Content\0000000000000000\C0DE9999\00080000\C0DE99990F586558` copied to the drive through the agent (our volume
wrote it), and the stick's launch.ini now says `Default = Hdd:\Content\...\C0DE99990F586558` with the agent still from
the stick, so a default the drive cannot serve falls back to the NXE dash and the agent stays. The run 9 module on the
stick, the same ROM, a shadow boot. Purpose: whether a volume mounted at the module's start (+1.65 s in run 9) is
there when DashLaunch resolves and launches its default item; the agent's `title` line says what came up (XeXMenu's
package or `\Device\Flash\dash.xex`), and the user sees it.

Result: the console came up in XeXMenu, title FFFF0055 at `\Device\Package_7C7B2A6BC9240B49FA4E2C1CCBBA3C02\default.xex`,
a package mounted from the drive's copy (`Hdd:` reaches nothing on the stick): DashLaunch resolved and launched its
default through our volume. The module's log (`exfat_drive_after_run10.log`, last block): no volume at +1 ms, our
volume at +1652 ms, the gate at +2653 ms with our volume still there, and 237 clusters (30 MB, the package) less free
than run 9. The gate came 500 ms after run 9's: the executable was the 30 MB package read from the drive, not the flash
dash. So a volume mounted at the module's start is there for DashLaunch's default.

## Runs 11 and 12: xam's look at the drive

The user asked whether xeBuild's `noSShdd` add-on was in the flashed image. It was not: the image's 78-record patch
set is patches_g2mcorona.bin's 74 kernel records plus hvFixKeys and exfat, and noSShdd's records at 0x15dafc and
0x15db08 are absent. It would not have mattered: the base set already holds `li r3, 1; blr` at 0x15d9d8, the entry of
the kernel's security-sector check (0x8015d9d8 reads 512 bytes at 0x2000, compares the serial, firmware, model and
size, verifies the signature, returns 1 at 0x8015dafc when all pass and 0 at 0x8015db08 otherwise; its caller at
0x8015dee4 lays the partitions out on 1), so the kernel takes any drive; noSShdd restores that entry and swaps the two
returns, which for a drive with no sector is the same 1. The sector decides nothing here.

The verdict is xam's, read from `17559_xam` in IDA: at its load, when the hardware has a drive, xam enters
`\Device\Harddisk0\Partition1` into its device table itself (0x8167ed90, twenty entries of 160 bytes at 0x81a9cb30:
name, id at +120, type at +124, state at +128) and looks once (0x8167e5b8): `NtQueryFullAttributesFile` on the
partition's root (0x816e0540) failing sets state 4, which the storage screen shows as unformatted; succeeding sets
state 3. xam loads at 0x800613b0, before the module's hook at 0x800613dc, so that look always met the kernel's FATX
refusal, whatever the module's mount point. The kernel's storage drivers announce sticks and the built-in memory unit
to xam through 0x8012bca8 with a record of six words (list links, the name as an ANSI_STRING, the action, the type),
which xam's worker (0x8167eb60, `DrvGetContentStorageNotification`) turns into a fresh entry and a fresh look.

Run 11 (`exfat_announce_run11.xex`, ea4d6e63): the module announces the partition again after its mount, with the
BuiltInMuMmc driver's record at 0x801a608c. That record is empty on this console (no 4 GB memory unit), so xam got a
nameless type 0 and ignored it (`exfat_drive_after_run11.log`); the dash came up. `tests/probe_storage_state.c`
then dumped xam's table from a title: the drive is **type 1**, state 4, and `\Device\Mass0\` type 16, state 3
(`probe_storage_state_run11/`). Type 16 is a stick, not the drive; xam dismounts types 16 and 2 before it looks,
not type 1.

Run 12 (`exfat_announce_run12.xex`, affaf468): the module announces with a record of its own, type 1, and declines
the partition's dismounts for two seconds after the announcement (xam dismounts a drive it listed as formatted when it
drops the old entry, 0x8167e760; it did not here, the old entry was state 4). The log (`exfat_drive_after_run12.log`):
our volume at +1953 ms, the gate at +2954 ms, then "announced to xam again, type 1" and no dismount. The probe
(`probe_storage_state_run12/`): the drive's entry is now `\Device\Harddisk0\Partition1\` id 4 type 1 **state 3**.
The user: "it appears in settings". The stick's launch.ini is back to the NXE default for these runs.

## The NAND flash and the plain boot after it

The user flashed the 18:22 image (the run 12 module, the exfat patches, hvFixKeys; the patch set otherwise the one of
the image flashed before). The plain boot (`exfat_drive_after_flash.log`,
last block): our volume at +1452 ms, the gate at +3237 ms, "announced to xam again, type 1", no dismount. The shadow
ROMs (`local/shadowboot/out/hdd|usb/xboxromw2d.bin`, the stick's copy) are rebuilt from that image. Copied to the
drive with the console agent, one connection, folder by folder: Aurora 0.7b.2 at `hdd:\Aurora` (167 files, 40566796
bytes, the package's own layout) and the DashLaunch 3.21 package at `hdd:\DashLaunch_v3.21` (10 files). Neither has
run from the drive yet.

## Runs 13 to 37: the sign-in that spins after a profile's creation

Creating a profile from the dash on the exFAT drive (first seen from Castle Crashers' sign-in prompt) sometimes leaves
the "creating profile" or "signing in" spinner forever: B gets out, the profile exists without its picture. It passed
on runs 14, 27, 29, 30, four times on run 36 and five on run 37; it spun on runs 25, 26, 33 and 35. Each run is one module build with
more instrumentation, kept here as `exfat_<what>_runNN.xex`. The trace goes to the stick
(`usb0:\XSDK\exfat\exfat_trace.log`, whose presence is also the trace flag), each line the writing thread and the
milliseconds since the module started; the slices kept here are `trace_run30_signed_in.log`, `trace_run33_spin.log`
and `trace_run35_spin.log`. What the runs established, in order:

- Run 13: the module's log lines queue and its thread writes them. The trace flag on the drive had frozen every boot
  it was on: a line logged under the module's lock on its own volume re-enters the module (`xsdk_plugin_log` writes
  through the kernel).
- Runs 17 to 19 (trace to the stick, caller chains): a hung attempt writes the same package sequence as a good one up
  to the close; the good run's next requests (a free-space query, two `NtQueryFullAttributesFile`, the mount) never
  come in a hung one, and the module logs no error.
- Runs 20 to 24 (thread sampler): the close returns, xam and the dash rebuild `ContentCache.pkg`, then no thread runs
  logon code. `XamUserLogon` itself works on the drive (tests/probe_signin.c signs the newest profile in within 2.7 s).
  xam's export table cannot be hooked, its image being hypervisor-protected (tests/probe_xam_exports.c); the module
  instead repoints the vtable words of xam's service object at traced copies (run 26: `XamUserLogon` and every task
  entry).
- Runs 25 to 32: the creation watched through xam's heap (task routine 0x816DC2B0 and its request); in a good run the
  dash's `tasks[8](815FB0B1, 5, ...)` follows `tasks[8](0, 0x9030, 1, 0)` by about 400 ms (run 30). The watcher
  outranks the dash's threads (run 31: the kernel clamps a system thread to +4 above the process base), a 2 MB heap
  scan costs about 90 ms (run 32), and a stick write blocks for seconds under the dash's I/O, so the trace's writer
  became a thread of its own behind a 3072-line queue (run 33).
- Run 33 (spun): a priority inversion on the module's lock. The module's thread (the plugin thread, base priority,
  hardware thread 2) had queued on the lock behind the dash's I/O, as its poll, log write and flag checks went through
  its own dispatch; `RtlEnterCriticalSection` handed it the lock, and xam's guide UI pump (task 0x816CE108 pumping
  0x816CD6E8 while a task is pending) kept hardware thread 2 at a higher priority for 36 s. The dash's directory
  create took 36.5 s; the creation finished at +157.6 s with result 0, but the dash had given up and never called
  `XamProfileGetCreationStatus` or `XamUserLogon` again.
- Run 34: the module's thread runs at the band's top (`KeSetBasePriorityThread(self, 16)`, clamped to +4: KTHREAD
  +0x70 priority, +0xB9 base, +0xC8..+0xCA min/base/max, saturation at |increment| >= 16), and the sampler prints each
  thread's priority, base priority and hardware thread. xam's pool threads run their tasks at 6 to 7 or at 15.
- Run 35 (spun): the module's thread looks its flag files up under a try-lock (`flag_present`) and never queues. The
  creation finished in 1.6 s; then the dash's logon thread waited 23 s in a create dispatch on a critical section
  (chain: `IopParseDevice`'s `IoCallDriver` at 0x8006C488, one frame, `RtlEnterCriticalSection`), its open never
  reached the module, and the dash gave up (overlapped status 0x65B, error 0x800703E3 at +261.2 s). Read against the
  kernel image after run 37, the wait is FATX's, on the stick, three threads deep, with the trace's own writer thread
  at its root. FATX's create dispatch (0x800A51C8) takes the volume's critical section at +128 and then its
  read-write lock at +16 shared (0x800A522C, 0x800A523C); a FATX write that updates the directory entry (0x800A3F20)
  takes the lock at +16 exclusive and holds it across the directory read (0x800A1B38). The writer thread (priority 5,
  hardware thread 2) sat in that write for 24 s, ready but never run behind xam's UI pump at priority 6 on the same
  hardware thread, holding the lock at +16; the module's thread (priority 9), in `file_exists` on the stick, held
  the stick volume's critical section at +128 and waited at 0x800A5240 for the lock at +16; the logon thread, opening
  a file on the stick, waited for the critical section at +128. All three went on when the dash cancelled the logon
  and the pump stopped (+261.0 s). So run 33's mechanism, the module's thread handed the module's lock while starved,
  is closed by runs 34 and 35, and run 35's spin was the instrumentation's own thread doing I/O at base priority on
  hardware thread 2. The sampler printed a thread only when its state changed, kept a slot for every thread it ever
  saw and stopped its walk at a bad link; one thread (3A0BE4C0, a kernel worker at priority 17) read the volume during
  the run and was never sampled.
- Run 36: the watcher samples the three critical sections' kernel words each loop (lock count +0x10, recursion count
  +0x14, owner +0x18, the dispatcher header's signal state +4 and wait list +8; `RtlTryEnterCriticalSection`
  0x80089184) and the holder `lock_module` notes; the writer thread runs at the band's top too. Four creations signed
  in; the lock changed hands within 0.4 s and was held 0.6 s at most.
- Run 37: while the lock stays held across loops, the holder's and every registered waiter's sample is printed once a
  second whether or not it changed; a thread's slot is freed when it leaves the lists; a walk that ends early says
  where. Five creations signed in: nine in a row on runs 36 and 37, after four spins in the seven attempts before.
- Run 38 (`exfat_clean_run38.xex`): the instrumentation out (the stick trace and its writer thread, the thread
  sampler, the xam hooks and watchers, the lock watcher, caller chains, the 300 ms flush sleep); the queued log, the
  thread's priority, `flag_present` and the flush passed down to the device stay. Trace lines go to the queued log.
  Booted through the shadow loop (image 224 KB, was 852 KB; the drive mounted at +1.5 s and announced); three
  creations signed in, twelve in a row since run 36. The module logged nothing through them: no errors, no dropped
  lines.

## The plain boot with the run 38 module from the NAND (2026-11-16)

The user rebuilt the NAND with `exfat.xex` the run 38 build and `bin\exfat.bin` unchanged (96 bytes, the same as a
fresh `make exfat_patch`); step 4's `verify_image.py` read the image first: our two patch records byte-identical to
`build/exfat.bin`, no record at 0x61388, the module's 61440 bytes in four contiguous blocks from 0xe2c000. The plain
boot: the module loaded from the NAND at 0x91580000-0x915B8000, the partition mounted at +1.4 s, the gate at +3.0 s,
the drive announced to xam, the dash up.

## Step 1 checks so far

- [x] The module with the fourth device boots through the shadow loop on a FATX drive (run 6).
- [x] The FATX partition is left alone: no mount, the kernel's volume on it unchanged (run 6).
- [x] The FATX boot timeline recorded (run 6).
- [x] The drive formatted exFAT by the title through the pad UI, the step 2 pad check with it (the user, 2026-11-15).
- [x] The exFAT drive's boot timeline (the NAND's module): no volume to the gate at +3.2 s, mounted at +4.6 s.
- [x] The mount point chosen from the two timelines: one mount attempt at the module's start, before the gate (run 9).
- [x] DashLaunch launches its default, XeXMenu's package, from the exFAT drive (run 10).
- [x] The dash's storage settings list the exFAT drive: the module announces the partition to xam after its mount
  (run 12, xam's entry state 3, the user's screen).
- [x] The post-format boot's missing module log explained: the format erased the log's folder while the module's
  log flag stayed set; the flag now clears on the partition's mount (runs 8 to 12 logged on the drive).
- [ ] The formatter's log kept when the title runs from a stick: the close now happens only when the title runs from
  the partition it formats (build b7a89dbc on the stick), unverified until the next format.
- [x] The NAND reflashed with the run 12 module; the plain boot mounts the partition and announces it (the log above).
- [ ] The plain boot's xam state read with the probe (state 3 expected, as in run 12).
- [x] launch.ini, a plugin and the Default from the exFAT drive: with the stick out, DashLaunch read `hdd:\launch.ini`
  (Default `Hdd:\Aurora\Aurora.xex`, plugin1 `Hdd:\XSDK\devagent.xex`), the agent answered at +41 s with Aurora as
  the title (`devagent_plain_boot.log`, 2026-11-15 evening).
- [ ] exfat_check on the drive; profiles and saves.
- [x] A profile created from the dash signs in every time: twelve in a row on runs 36 to 38 with the fixes of runs 34
  and 35, the last three on the clean module, against four spins in the seven attempts before (runs 13 to 38 above).
- [x] The NAND reflashed with the run 38 module: the plain boot mounts the partition and announces it (the section
  above).
