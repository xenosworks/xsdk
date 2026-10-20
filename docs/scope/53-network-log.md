# Milestone 53: the log over the network

Proposed on 2026-10-20, not yet agreed.

## Why

A title's log goes to a file beside the `.xex` and to `DbgPrint` (src/log.c, `write_line`). `DbgPrint` reaches nothing
without Microsoft's XBDM and xbWatson, so a porter sees the log only by copying the file back after the title ends.
The console agent follows the file while a title runs, but it's local tooling built with the XDK and isn't part of
xsdk; it also polls every 0.5 s, so a hang that takes the console down loses up to half a second of lines.

docs/PARITY.md lists the log over the network as missing, and with the crash handler last it's the main way to see
why a title died.

## What it takes

1. **The title's side,** in src/log.c, opt-in:
   - At `xsdk_log_open`, if `net_log.txt` is beside the `.xex`, holding the PC's address and a port as `net_host.txt`
     does ("192.168.0.125 7371"), the log starts the network and opens a UDP socket to it. Without the file, nothing
     changes and the network isn't started.
   - Every line the file gets, `xsdk_log_write`'s and `xsdk_log_write_bytes`'s (stdout and stderr), also goes out as
     one datagram: a 32-bit sequence number, then the same bytes. The first line goes out as the file's does.
   - UDP, so the title never waits on the PC: a missing or slow PC drops lines, and the sequence numbers show where.
     A send that fails is counted, not retried, and the log carries on.
2. **The PC's side,** `tools/net_log.py`: listens on the port, prints each line as it arrives, writes each run to a
   file of its own (a new run starts where the sequence goes back to 0), and reports gaps.
3. **Docs:** docs/GETTING_STARTED.md, "Reading the log", and docs/GUIDE.md give both sides; docs/PARITY.md's row.

Left out: TCP and a reliable stream, which would need a sender thread so the title never blocks; the console agent
following the stream in place of the file; logging from the PC back to the console.

## Verification

On the console, through the agent, `tools/net_log.py` running on the PC:
- **Equal:** xsdk_test with `net_log.txt`: what net_log.py writes equals, byte for byte, the log file the agent
  fetches, no gaps; xsdk_test passes as without it.
- **Off:** without `net_log.txt`, nothing arrives, and the log says the network log is off.
- **Nobody listening:** `net_log.txt` naming a port nothing listens on, xsdk_test passes as without it.
- **The cost:** a check logs 1000 lines with the stream on and off and logs the time a line takes each way, to
  set the bar from (a few tens of microseconds a line is the guess).
- **Broken on purpose:** a build that drops every 100th datagram, which net_log.py must report as gaps and the
  byte-for-byte comparison must fail.

vulkan4xenon_test twice and vkbench, unchanged.

## Done when

- A title with `net_log.txt` streams every line of its log to `tools/net_log.py` as it writes it, byte for byte the
  file, at a measured cost a line.
- A title without it is as before, and one whose PC isn't listening runs as before.
- The docs say how, and docs/PARITY.md marks the log over the network Yes.
