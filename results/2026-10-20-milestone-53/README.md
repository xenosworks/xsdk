# Milestone 53: the log over the network

docs/scope/53-network-log.md; docs/HARDWARE.md, "The log over the network, milestone 53". Built clean with
local/m53/build.sh at b0b2327 and run through the console agent on 2026-10-20 with `tools/net_log.py` and
`tools/net_echo.py` running on the PC.

## What was built

- **src/log.c:** with `net_log.txt` beside the `.xex` ("192.168.0.125 7371"), every line the log file gets also goes
  to the PC as one UDP datagram, a 32-bit sequence number and then the same bytes. The socket opens before the log
  file, so the PC gets every line. A failed send is counted and reported as the log closes.
- **`DbgPrint`** gets the log only with `dbgprint.txt` beside the `.xex`, or after `xsdk_log_set_dbgprint(true)`
  (include/xsdk/log.h), which returns the setting it replaces. It was on for every line before, at 5.6 ms of a line's
  5.9 in the first runs below.
- **tools/net_log.py** prints each line as it arrives, writes each run to a file of its own, and reports lines that
  never came.
- **xsdk_test** times 100 lines of the log, `log: a line costs under 1500 us`, then 10 with `DbgPrint` on through the
  call, which no bar holds.

## On the console

| Run | xsdk_test | A line, as the title runs | With `DbgPrint` on | What reached the PC |
|---|---|---|---|---|
| `net_log.txt` naming the PC (`xsdk_test_on.log`) | 534 of 536 | 434 us | 6,885 us | `net_log_on.log`, the log file byte for byte, 67,379 bytes |
| The drop break (`xsdk_test_drop.log`) | 534 of 536 | 320 us | 2,982 us | lines 99, 199 to 899 reported missing; 66,866 bytes against the file's 67,390 |
| `net_log.txt` naming a port nobody listens on (`xsdk_test_nobody.log`) | 534 of 536 | 318 us | 2,980 us | nothing, as expected; no send failed |
| No `net_log.txt` (`xsdk_test_off.log`) | 534 of 536 | 309 us | 2,965 us | nothing |
| `dbgprint.txt` (`xsdk_test_dbgprint_txt.log`) | 533 of 536 | 6,416 us, failing the bar | 5,505 us | nothing |
| `DbgPrint` on by default, the break (`xsdk_test_dbgprint_on_break.log`) | 533 of 536 | 5,043 us, failing the bar | 5,504 us | nothing |

- Each xsdk_test run failed the controller's two checks with the pad idle, and the `DbgPrint` runs the cost bar too.
- **The drop break** drops every 100th datagram and still counts it. `net_log.py` reported each gap
  (`net_log_out.txt`, from 18:09:49 on; the lines before are the runs of the step that found `DbgPrint`), and its file
  differs from the log.
- **The break that starts `DbgPrint` on** logs "as dbgprint.txt asks" without the file there, as the note is written
  wherever `DbgPrint` starts on; only that build starts it so.
- vulkan4xenon_test 517 of 517 twice, and vkbench 17 of 17, xsdk's draw+submit within 1% of milestone 50's.

**What a line costs:** about 0.31 ms to the file, and up to 0.13 ms more over the network, single runs of 100 lines
each. `DbgPrint` costs 3 to 7 ms a line on this console. The step that found it, with every line still going to
`DbgPrint`: 5.94 ms a line, and 0.31 with `DbgPrint` taken out (b0b2327's message). Why it's slow here isn't known.

## Not done

- A console hang that takes the agent down, to show the stream keeps the last lines the file loses: it would need a
  power cycle by hand.
- The console agent following the stream in place of the file.
