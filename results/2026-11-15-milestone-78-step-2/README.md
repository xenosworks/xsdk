# Milestone 78 step 2: the formatter title on a USB stick

docs/scope/78-exfat-hdd.md, step 2: `exfat_format.xex` (tests/exfat_format.c) formats a device exFAT from the console.
Console on kernel 2.0.17559.0, booted from the NAND that loads the exFAT module (milestone 77 step 4); the stick is the
user's 61.5 GB stick in USB0, exFAT from the PC before, holding GoW2Hollow.zip and exfat_check's files, erased with
the user's word.

## Runs

| Run | Log | What it shows |
|---|---|---|
| Listing only: no pad, no `exfat_format.txt` | exfat_format_list.log | The device list, 5 s on screen, exit 0 in 8.8 s. `\Device\Mass0` type 45, 120176640 sectors, exFAT (MBR); Mass1 and Mass2 present without media (0xC00002B6); `\Device\Harddisk0\Partition1` **device type 7**, 302590256 sectors of 512, 154.9 GB, FATX. |
| Headless format: `exfat_format.txt` = `\Device\Mass0 XSDK` | exfat_format_mass0.log | Dismount 0xC000026E (nothing was mounted), the format, the module wait (1.5 s, nothing mounted it: the module was parked, below), the title's own read-back: 469400 clusters of 128 KB, 469397 free. Exit 0 in 4.1 s with the 1 s splash. |
| The stick on the PC | chkdsk_mass0.txt | Read-only chkdsk: "found no problems"; exFAT, label XSDK, 469400 allocation units of 131072 bytes, partition 1 at offset 1048576, MBR type 7. Windows took 3 clusters for System Volume Information. |

The request file was deleted from the console after the format, so later runs of the title only list.

## The module during a format

Reading the module for the title showed a race: the module's 500 ms poll read sector 0 before taking the device's
mount lock, so during a format it could mount the old volume from the stale sector 0 and, on its next sync, write over
the new one; and a device it had refused once stayed refused until the kernel mounted it, so a stick the title had
just formatted did not mount until replugged. plugins/exfat/exfat_module.c now reads sector 0 under the mount lock
and tries a device nothing has mounted at every poll; the title holds that lock through the format, as the kernel
holds it to mount (plugins/exfat/kernel_seam.md, section 3).

The NAND still carries the previous module. To swap the rebuilt one in, `exfat_unload.txt` was put in
hdd:\XSDK\exfat; the module dismounted the stick ("3 file objects were open") and then waited to unload
(exfat_module_unload_wait.log): its volume device stayed referenced by those three file objects through a title
launch (Aurora ended and came back) and through the stick's removal. Their owner is not Aurora and does not react to
the stick leaving; the step 3 record had only pulls with no file open. The console was rebooted with the stick out:
the NAND's module found the leftover `exfat_unload.txt` at boot and unloaded at once with nothing mounted
(exfat_module_swap.log), exfat_load.xex loaded the rebuilt module from hdd:\XSDK\m77, and the stick went back in.

## The stick after the format, through the rebuilt module

| Run | Log | What it shows |
|---|---|---|
| The stick plugged back in | exfat_module_swap.log | `\Device\Mass0: mounted exFAT "XSDK" serial 0BB75C00: 120172544 sectors of 512 from sector 2048, 128 KB clusters, 469394 of 469400 free`. |
| exfat_check.xex, milestone 77 step 3's checks, on the title-formatted stick | exfat_check_formatted_stick.log | **157 passed, 0 failed**, 384 s. Leaves exfat_check\verify.bin and big.bin on the stick as before. |

## Step 2's checks

- A stick formats from the title and passes step 3's checks: done (above).
- The drive formats from the title and passes step 1's checks: waits for the user's spare drive.
- A device name in the file that holds no media is refused: not run (the code path logs and exits 1; a run with
  `\Device\Mass1` would show it).
- The title with neither pad nor file only lists: done (the first run).
- The drive's extra confirmation refuses a single A: needs a person with the pad, with the spare drive.

Left out of this record: who held the three file objects; the pad UI.
