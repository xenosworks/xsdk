# Milestone 77, step 3: the exFAT module on the console

Console: the user's RGH, kernel 2.0.17559.0, the USB0 stick formatted exFAT in step 1 (120,176,640 sectors of 512;
the volume from sector 2048, 128 KB clusters, 469,400 clusters, label XSDK, serial 643FC9ED). All on 2026-11-14.

## What ran

| Folder | Title | Purpose | Result |
| --- | --- | --- | --- |
| load | exfat_load.xex | first load of plugins/exfat/exfat_module.c (base 0x91580000) | image 0x91580000-0x9167B000; the raw volume dismounted, the stick mounted ("469397 of 469400 free") |
| check | exfat_check.xex | the scope's file checks through xsdk's calls, 132 of them | 132 passed, 0 failed, 465 s; big.bin (4097 MiB) at 8 MiB/s |
| corrupt | exfat_corrupt.xex + loads | a boot sector damaged on purpose is refused | byte 0x20 of the boot sector 00 → FF: the next load logs "exFAT volume refused: not exFAT", nothing mounted; mended, the next load mounts again |
| speed .. speed15 | probe_mass_speed.xex | why the module wrote at 8-9 MiB/s when raw IRPs from a title do 15 | see below |
| check_final | exfat_check.xex | the same 132 checks on the final module build | **132 passed, 0 failed, 362 s**; big.bin at 11 MiB/s (15 for its first GiB, 10-11 after) |
| pc | chkdsk, check_stick.py | the stick on the PC: file system sound, files byte-identical | **no problems, all identical** (below) |
| speed16 | probe_mass_speed.xex | 2 GiB through the module against 2 GiB raw, timed every 512 MiB | module 2048 MiB in 171.0 s (11 MiB/s), raw 204.0 s (10 MiB/s): the stick itself had slowed by then, after some 12 GB of writes in the hour; the module is not behind raw |

exfat.log in each folder is the module's log (hdd:\XSDK\exfat\exfat.log) as fetched after that run.

## The write speed

Raw `IoSynchronousFsdRequest` writes from a title run at 15 MiB/s whatever the IRP size (64 KB, 256 KB, 1 MiB; speed,
speed2), reads at 17; isolated 512-byte and 4 KB writes cost 1-2 ms (speed3). The module's file writes ran at 8-9 MiB/s
(speed4 to speed12), and its own timing put the loss inside its device IRPs, 6-9 ms each against 4.1 raw. Ruled out,
one console run each: metadata writes between the data (a sync every poll against one every 5 s: 8.6 against 9 MiB/s,
speed5, speed6); anything between a stream's IRPs (the poll's IOCTLs, a 4 KB write far away, a 1 ms sleep: all 15
MiB/s raw, speed7); the place on the stick (raw writes over the very clusters the module's file took: 15, speed8);
1 MiB IRPs instead of 64 KB (9, speed10); the kernel's critical region around the file system call (a title's raw
sweep inside one: 15, speed11); the kernel's page lock for the title's buffer (the module without the copied
lock-pages flag: 9, speed12); 512-byte or 4 KB sector rewrites between raw writes (15, speed14).

What it was: the module's poll thread, every 500 ms, waited for the module lock while a stream's dispatch held it. A
poll every 10 s gave 14 MiB/s (speed13); the poll skipping a taken lock (`RtlTryEnterCriticalSection`) gives 15
(speed15: "256 MiB in 16702 ms, 15 MiB/s", raw 16761 ms the same run). Why a waiter on the lock slows the owner's
later device IRPs is not understood; it is a kernel scheduling matter, and the module simply never waits in its poll
now. The histogram build (speed9) showed the loss as 74 IRPs over 30 ms and some 1,200 at 5.5-10 ms out of 4,148, not
a uniform cost.

Side effect to know: the speed9 probe's raw sweep began where the module's file had begun, and the module, just
reloaded, had put that file's first MiB in a hole near the heap's front (its allocation hint resets at a mount), so the
sweep overwrote clusters 6-2053 and the front of big.bin with them. check_final rewrote big.bin; the stick's files
are the ones that run left.

The stick's own speed moves: raw writes ran at 15 MiB/s in every 256 MiB sweep of the morning and at 10 over the
2 GiB sweep of speed16, after the day's writes; big.bin's drop from 15 to 10-11 after its first GiB (check_final) is
the stick's. Judge the module against raw in the same run, never against another run's number.

## On the PC (pc/)

The stick moved to the PC as E:\ after check_final, with Windows 11 as the judge, exfatprogs's fsck being out of reach
for a physical stick from WSL:

- `chkdsk E:` (pc/chkdsk.log): "Windows has scanned the file system and found no problems"; 4 files in 4,195,712 KB,
  436,616 of 469,400 allocation units free, the same count the module reported after check_final. Windows mounted the
  volume as it is and wrote its System Volume Information folder onto it.
- `check_stick.py E:\` (pc/check_stick.log): verify.bin identical; big.bin 4,294,971,392 bytes (the 4 GiB + 4096 cut)
  with 0 bad pieces of 4097; exfat_check holds those two files alone. **RESULT all identical.**

So what the module wrote is what a PC reads, and the structure it left is one Windows finds sound.

## Left for the user, per the scope

- Pull the stick while mounted and plug it back: exfat.log should show "dismounted (media gone)" and then "mounted".
- Aurora's file manager lists the stick and copies to and from it.
- Aurora's FTP uploads and downloads a file over 4 GB, the speed logged against FATX on the same stick.
- The dashboard with the exFAT stick in (left from step 1): does it offer to format.

## Known limits

- xsdk's `off_t` is 32 bits (newlib's `_off_t`), so lseek, pread, pwrite, ftruncate and stat can't address past
  2 GiB; `write` tracks a 64-bit position, so sequential writes past 4 GiB work (exfat_check uses the Nt calls for
  64-bit offsets). An SDK matter, not the module's.
- The module waits for all its volume devices to be deleted before it unloads, so a title holding a handle to the
  stick delays the unload until it closes it (logged every 10 s).
