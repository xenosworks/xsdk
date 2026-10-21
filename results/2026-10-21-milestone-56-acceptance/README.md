# Milestone 56 on the console (2026-10-21)

Each title's log, unchanged, from runs by the console agent against the M56 worktree's tools/net_echo.py and
tools/net_http.py, both on Windows' own Python, which wrote net_http.py's logs with CRLF line endings; they're kept
with LF, as git stores them. [SHA256SUMS](SHA256SUMS) has their hashes. docs/HARDWARE.md,
"Milestone 56 on the console", says what they found.

## xsdk (`xsdk/`)

- [xsdk_test](xsdk/xsdk_test/xsdk_test.log) at `d832a03`: 618 passed; the 2 failures are the controller's state and
  rumble, with no one at the controller.
- [launch_check](xsdk/launch_check/launch_check.log): 5 of 5, the launch through xsdk's link from outside its folder
  among them; [launch_target](xsdk/launch_check/launch_target.log) beside it.
- vulkan4xenon_test 520 of 520 [twice](xsdk/v1/vulkan4xenon_test.log) ([second](xsdk/v2/vulkan4xenon_test.log)) and
  [vkbench](xsdk/bench/vkbench.log) 17 of 17, built against M56's xsdk.
- The 14 breaks of [cases.tsv](../../tests/breaks/m56/cases.tsv), one folder each. Each fails the checks its line
  names: `no_enoent` and `no_link` by launch_check never logging its end. The xsdk_test breaks ran before the three
  checks below were corrected, except `limits_dropped`, `linger_unconverted` and `poll_pri`, which ran after, as those
  checks had hidden them; the others' logs show those three failing too.

Three checks were corrected during the runs, the code unchanged: the linger check now expects what XAM does (the PC
sees no FIN), the buffer check at least the raised size (XAM gives 65700 bytes for 63 KB), and the POLLIN check polls a
connection of its own, as net_echo.py closes each after one line.

## curl-xenon (`curl/`)

curl_check from curl-xenon `f65d5550b`, against xsdk at `8c048de` and later:
- [curl_check](curl/curl_check/curl_check.log): 13 of 13, the round trip first: launch_target downloaded, its SHA-256
  checked, launched, and launching curl_check back with the data reversed.
- Ten breaks, one folder each, `server_` for net_http.py's `--break` and the rest named in `curl_check_break.txt`:
  each fails its own check, and `server_byte` also those whose files it changes (HTTPS, redirect, catalogue, resume).
- In the first run, not kept, net_http.py's HTTPS reset one download being written to `hdd:` and then stopped taking
  connections; it didn't recur in nine runs after, nor in a slow reader on the PC alone.

Two runs left the console needing a reboot: after a long session three of milestone 55's mutant checks failed, and
after about an hour of launches the agent's launch returned 1 with no title starting. Each passed again after the
reboot (TODO.md).
