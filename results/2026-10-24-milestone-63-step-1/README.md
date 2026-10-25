# Milestone 63, steps 1 and 2, on the console (2026-10-24)

Each title's log, unchanged, from runs by the console agent, xsdk_test's against tools/net_echo.py and curl_check's
against tools/net_http.py, both on Windows' own Python; net_http.py's log is kept with LF. [SHA256SUMS](SHA256SUMS) has
their hashes. docs/HARDWARE.md, "Zero runs in a .xex, milestone 63" and "A store's update and its downloads, milestone
63", says what they found.

## Zero runs (step 2)

xsdk_test's ELF, packed five ways, each run from `hdd:\XSDK\m63` under its own name:
- [x_raw](x_raw/x_raw.log): one raw block, the packing before; 634 passed.
- xsdk_test with 4 KB runs, its last block ending in the image's 6.6 MB of zeros: no log. The agent's launch returned 1,
  as every launch does, and the console was back on Aurora.
- [x_tool](x_tool/x_tool.log): XexTool's `-c u -e u` of that file, two blocks at 32 KB, none for the zeros at the end;
  634 passed.
- x_g32, 32 KB runs as XexTool's, the zeros at the end in the last block: no log, as the second.
- [x_tail](x_tail/x_tail.log): 4 KB runs, the zeros at the end in no block; 634 passed.

Then with tools/elf2xex.py writing the last block without zeros:
- [xsdk_test](xsdk_test/xsdk_test.log): 634 passed; the 2 failures are the controller's state and rumble, with no one at
  the controller.
- [swanstation](swanstation/swanstation.log): SwanStation (swanstation-xenon `f935e9e2`) from `hdd:\swanstation`, 30 s:
  the picker up, the games folder read.

## Probes (step 1)

- [probe_replace](probe_replace/probe_replace.log): version 1, with version 2 beside it as `probe_replace.new`, renames
  it over its own running `.xex` and launches it; the log is version 2's, holding version 1's notes.
- [probe_replace_nonew](probe_replace_nonew/probe_replace.log): version 1 alone, which launches nothing.
- [curl_check](curl_check/curl_check.log), curl-xenon `f65d5550b` with the GitHub rate added, from `hdd:\XSDK\m63c`:
  13 of 13, then the rates, the GitHub release's last; [net_http.log](curl_check/net_http.log) beside it.
