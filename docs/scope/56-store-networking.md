# Milestone 56: a homebrew store's networking

Proposed on 2026-10-20, not yet agreed.

## Why

docs/scope/networking.md named a homebrew store as the first use of xsdk's sockets and TLS, and left an HTTP library to
a scope of its own. A store lists what it offers, downloads it to the hard disk or a USB drive, starts it, and is come
back to. xsdk has the sockets, `getaddrinfo` and TLS 1.2 (mbedtls-xenon) for that. The rest is missing, and
docs/PARITY.md lists an HTTP client as TLS 1.2's missing piece and launching another `.xex` as No:
- **No HTTP client.** mbedtls-xenon's check writes its one request by hand (programs/xsdk/https_check.c there). XAM's
  own HTTP calls, `NetDll_XHttp*` from 0xC9 (xenia:src/xenia/kernel/xam/xam_table.inc:137), have no code in Xenia and
  rest on XAuth and Xbox Live (unverified here; left out with Live either way).
- **No JSON** for a catalogue.
- **No link status or address.** Only tests/probe_net.c calls `NetDll_XNetGetEthernetLinkStatus` and
  `NetDll_XNetGetTitleXnAddr`, by ordinal. On the console they gave 0x0B, 100 Mbit/s full duplex, and 192.168.0.227
  over DHCP (docs/HARDWARE.md, "Sockets reach the LAN and the internet when marked insecure"). A store wants to say
  there's no cable, and show its address.
- **No socket timeouts or linger.** sys/socket.h has no `SO_RCVTIMEO`, `SO_SNDTIMEO` or `SO_LINGER`, so a blocking
  `recv` from a stalled server waits forever. In the option scan XAM's `getsockopt` answered nothing for 0x1005 and
  0x1006, Winsock's timeouts. It answered 0x0080, Winsock's linger, with 60, which reads as Winsock's two 16-bit fields;
  POSIX's `struct linger` is two ints (HARDWARE.md, "Sockets in xsdk").
- **XAM's limits as they come.** xsdk starts the network with every setting 0 (src/socket.c, `start_network`):
  - The option scan read 16,384 bytes of send buffer and 17,520 of receive.
  - With `close` broken on purpose, a socket failed with `ENOBUFS` at 33 open (the same section).
  - A 17,520-byte window holds one connection to about 0.6 MB/s at a 30 ms round trip.
  - `XNetStartupParams` carries a title's socket counts and buffer sizes in KB
    (xenia:src/xenia/kernel/xam/xam_net.cc:154-168); whether XAM honours them is for the console to say.
  - xsdk's own table holds 29 sockets: `FD_SETSIZE`, newlib's 64, less the 35 descriptors before them (src/socket.c).
- **No measured download rate** to plan a store's downloads by.
- **No launching.** `exit` goes only to the dashboard (src/exit.c), and `main` gets one empty argument (src/start.c).

What Xenia shows, little of it the console's:
- **The link and the address** are stubs, giving no link and loopback (xam_net.cc:452-476, 529-540).
- **`setsockopt`** passes every option but 0x5801 and 0x5802 to the host's sockets unchanged
  (xenia:src/xenia/kernel/xsocket.cc:76-95), so it says nothing of XAM's.
- **`NetDll_XNetGetOpt`** option 1 gives the startup settings back (xam_net.cc:206-223, sketchy).
- **The loader calls,** all sketchy (xenia:src/xenia/kernel/xam/xam_info.cc:247-322):
  - launch data is kept in XAM across a launch, and never cleared;
  - `XamLoaderLaunchTitle` never returns; a bare name runs from the title's own folder, and an empty one is
    `game:\default.xex`;
  - Xenia starts the next title only if launch data was set (xenia:src/xenia/emulator.cc:533-544,
    app/xenia_main.cc:652-653).
- **`ExLoadedCommandLine`,** xboxkrnl's variable 0x1AE: Xenia fills it with `"default.xex"` and its `--cl` option
  (xboxkrnl_module.cc:187-204). What the console's loader puts there isn't known.

What libcurl needs, from its docs and sources, and what xsdk has of it:
- **Sockets,** non-blocking connects finished through `poll` or `select`, and `SO_ERROR` after one
  (curl:lib/cf-socket.c, `verifyconnect`). xsdk gives 0 until XAM's select reports a failure, then `EIO` (src/socket.c).
- **`getaddrinfo`,** and Mbed TLS 3.2 or later for TLS (curl.se/dev/internals.html). mbedtls-xenon is 3.6.7.
- **A way to wake `curl_multi_poll`:** an eventfd, a pipe or `socketpair`, none of which xsdk has, or failing them a TCP
  pair over 127.0.0.1 (curl:lib/socketpair.c, `wakeup_inet`). `CURL_DISABLE_SOCKETPAIR` leaves it out
  (curl:docs/CURL-DISABLE.md). Whether XAM connects a title to itself over loopback is a console question.

cmake/xsdk.cmake links every `try_compile` against libxsdk, so curl's configure checks see what a title gets.

## What it takes

After milestone 54, whose `fsync` and drive names (`hdd:`, `usb:`) a download uses. Each step probes on the console
first where Xenia can't answer.

1. **A probe,** tests/probe_net.c extended: its cases from a start file, every wait with a timeout, against
   tools/net_echo.py. It settles:
   - the link status and the address right after the network starts, and whether the address is pending at first;
   - the limits: what `NetDll_XNetGetOpt` reads after a start with every setting 0, and after one asking for more stream
     sockets and bigger buffers; how many TCP sockets open at once each way, and what `SO_RCVBUF` and `SO_SNDBUF` read;
     and whether `SO_RCVBUF` raised on one socket does the same;
   - whether `NetDll_XNetCleanup` (0x34) and a second `NetDll_XNetStartup` take new settings, as the network log
     starts the network before `main` (src/log.c);
   - `SO_RCVTIMEO` and `SO_SNDTIMEO` given as Winsock's milliseconds: whether XAM takes them; when a blocking `recv`
     from a silent PC and a `send` into one that stopped reading return, with which error; and what's left of the
     socket after;
   - `SO_LINGER`'s layout, and whether {1, 0} resets the connection as the PC sees it;
   - a TCP connection from the title to its own listening socket on 127.0.0.1;
   - the loader, in two probe titles, as a launch ends the one launching:
     - the most launch data `XamLoaderSetLaunchData` (0x1A6) takes;
     - whether it reaches the launched title whole, through `XamLoaderGetLaunchDataSize` (0x1A7) and
       `XamLoaderGetLaunchData` (0x1A8), and whether it's still there for a third title, or after the dashboard;
     - which paths `XamLoaderLaunchTitle` (0x1A4) takes: `game:\`, a bare name, `\Device\Harddisk0\Partition1\...`
       and `\Device\Mass0\...`; and what it does with a path to nothing;
     - whether it must come after the GPU and the display go back to XAM, as `exit`'s does (src/exit.c);
     - what `ExLoadedCommandLine` holds, launched by the console agent, by the probe, and once by you from Aurora.
2. **The link and the address:** imports 0x4B and 0x49, `XNADDR` in kernel.h (xam_net.cc:51-58), and in `xsdk/net.h`:
   - `xsdk_net_link`: XAM's link bits, named (xam_net.cc:529-535);
   - `xsdk_net_address`: the console's IPv4 address, or `EAGAIN` while XAM is still getting one, as the probe shows.

   Both start the network, as a socket does.
3. **Socket options,** in src/socket.c and sys/socket.h:
   - `SO_RCVTIMEO` and `SO_SNDTIMEO` take POSIX's `struct timeval`. If the probe shows XAM honours them, xsdk passes
     milliseconds. If not, xsdk keeps them with the socket and waits through `NetDll_select` before a blocking `recv`,
     `send` or `accept`, failing with `EAGAIN` when time's up, as POSIX has it.
   - `SO_LINGER` takes POSIX's `struct linger`, turned into Winsock's two 16-bit fields and back.
   - `getsockopt` reads each back as set.
4. **The network's limits:**
   - A title that wants more defines `xsdk_network_limits`: stream and datagram sockets, receive and send buffers in
     KB. xsdk reads it as it starts the network, which is before `main` where the log starts it. libxsdk's own, weak, is
     all 0, XAM's defaults.
   - If the probe shows XAM ignores the buffer sizes, xsdk sets them on each socket as it marks it insecure.
   - The socket table grows to the most the probe shows XAM holds. Sockets numbered past 63 work with `poll` and the
     descriptor calls, not with `select`, whose `fd_set` holds 64.
5. **Launching,** in `xsdk/xam.h`, with the loader's imports above and `ExLoadedCommandLine` (0x1AE):
   - `xsdk_launch(path, data, size_bytes)`:
     - the path goes through src/path.c, so `game:` and milestone 54's names, which only xsdk knows, reach XAM as
       device paths;
     - the data is set, then the title leaves as `exit` does: `atexit` handlers, files, GPU, display and log, ending in
       `XamLoaderLaunchTitle` in place of `XamLoaderTerminateTitle`;
     - it fails before anything ends: `ENOENT` if the `.xex` isn't there, `E2BIG` past the probe's maximum.
   - `xsdk_launch_data(buffer, size_bytes)`: the launch data the title started with, or none.
   - `argv` from `ExLoadedCommandLine`, split in src/start.c as the probe shows it laid out. If the loader leaves only
     the image's name, launch data is how a title gets arguments, and docs/GUIDE.md says so.

   A launched title comes back by launching the store again, with a path the store gives it (decision 3).
6. **curl-xenon** (decision 1): curl's latest release, built through cmake/xsdk.cmake on mbedtls-xenon
   (`CURL_USE_MBEDTLS`), static, without the curl tool:
   - HTTP and HTTPS only: every other protocol off (`CURL_DISABLE_*`), and proxies, cookies, netrc, Alt-Svc and HSTS;
     no IPv6, zlib, brotli, zstd, nghttp2, libpsl, libssh2 or libidn2;
   - no built-in CA bundle: a title passes `CURLOPT_CAINFO`, `cacert.pem` beside its `.xex`, as mbedtls-xenon's checks
     do;
   - curl's threaded resolver on xsdk's pthreads if it builds, so curl's connect timeout covers a lookup, which xsdk's
     `getaddrinfo` can hold for 30 s (src/netdb.c);
   - `CURL_DISABLE_SOCKETPAIR`, unless the probe shows loopback works;
   - changes under `__XSDK__` only where curl's checks can't see the platform, and docs/README-xsdk.md as
     mbedtls-xenon's.

   Its check title, `curl_check` (tests/xsdk/ there), walks a store's path against tools/net_http.py.
7. **JSON:** cJSON 1.7.19 (decision 2), as upstream ships it. It's plain C89 over `malloc` and the C library, so it
   should build through cmake/xsdk.cmake unchanged; this step is that build. `curl_check` reads its catalogue with it,
   and docs/GUIDE.md says how a title adds it.
8. **A download written safely:** a pattern in docs/GUIDE.md and `curl_check`, not a new call.
   - The download goes to `<name>.part` beside the final name, both within FATX's 42 characters (HARDWARE.md, "Shaders
     compiled as the game asks").
   - Then `fsync`, and `rename` over the final name, which replaces it (HARDWARE.md, "Files renamed on the console").
   - A dropped download resumes from the part's length with a `Range` request.
9. **The download rate,** measured by `curl_check` and recorded in HARDWARE.md:
   - HTTP and HTTPS from the PC, 8 MB each, into memory and onto `hdd:`, with XAM's buffers and with raised ones;
   - HTTPS from a public host (decision 5), with the cipher suite.

   The link's 100 Mbit/s is 12.5 MB/s at most. The numbers say which holds a download back: the link, the window, TLS
   or the disk.
10. **tools/net_http.py,** standard library only, beside net_echo.py. It serves, over HTTP and over HTTPS on a second
    port:
    - `catalog.json`, each file's name, size and SHA-256, and a malformed catalogue;
    - files of known bytes, any size, with `Range`;
    - a redirect, a response that stalls after its headers, and one that drops its connection halfway;
    - build/launch_target.xex, for the round trip.

    Its HTTPS uses Mbed TLS's own test certificate and key for `localhost` (server5, under test-ca2, in mbedtls-xenon's
    framework/data_files, good until 2029), so nothing new is made or committed. `curl_check` trusts that CA and sends
    `localhost` to the PC's address with `CURLOPT_RESOLVE`, so the name is checked too. `--break` options give the
    breaks below.
11. **Docs:** docs/GUIDE.md's sockets section, and sections on downloading and launching; docs/PARITY.md's rows;
    curl-xenon's docs/README-xsdk.md.

Left out:
- **Xbox Live and anything touching it:** XAM's XHttp, signing in, Live's secure layer and its services.
- **Anything for online cheating:** nothing here reads, writes or patches another title.
- **System Link** and secure sockets, which are for LAN play, not a store.
- **HTTP/2 and 3, TLS 1.3, compressed transfers** (a store's files come compressed), proxies, cookies and IPv6.
- **Unpacking archives and installing into content packages:** the store's own choice. A store writes a folder of
  files and launches its `.xex`.
- **Setting the console's clock.** Certificates are checked against it (src/time.c reads `KeQuerySystemTime`), so a
  wrong clock fails every HTTPS fetch, with curl naming the certificate's date. An SNTP query would be an item of its
  own.
- **Downloads that outlive the title, and power lost during one.** The part file keeps the old file through a dropped
  connection; FATX through a power cut isn't tried.
- **A store with a screen** (decision 4).
- **A wireless adapter's link bits:** the console is wired; they're logged if seen.
- **`getnameinfo` and `gethostbyname`,** which curl doesn't call.

## Verification

On the console through the agent, tools/net_echo.py and tools/net_http.py running on the PC. Each check is its own line,
and each is broken on purpose once.
- **xsdk_test,** with `xsdk_network_limits` raised. net_echo.py takes two new lines: `hold <ms>` keeps a connection
  silent and unread that long, then closes it; `last` says how the last held one ended.
  - The link is active, and the address is what the PC sees (net_echo's `addr`). Broken: the address's bytes swapped.
  - A blocking `recv` with a 200 ms `SO_RCVTIMEO` on a held connection fails with `EAGAIN` within 300 ms. Broken: the
    timeout dropped, so it returns at the hold's end.
  - A blocking `send` with `SO_SNDTIMEO` into a held connection fails with `EAGAIN` once the PC's window fills. Broken
    the same way.
  - `getsockopt` reads both timeouts and a linger back as set, and after `SO_LINGER` {1, 0} the PC reports a reset.
    Broken: `struct linger` passed unconverted.
  - One more TCP connection to the PC is open at once than XAM's default allows, and `SO_RCVBUF` reads the raised size.
    Broken: the limits not passed.
  - Launched by the agent, `xsdk_launch_data` finds what the probe found such a title gets: none, if XAM clears it.
    Broken: it reports data whatever XAM says.
- **launch_check and launch_target** (decision 3): two titles, each with a title ID of its own, unattended. launch_check
  launches launch_target with data and launch_check's own path. launch_target logs its `argv` and data, reverses the
  data and launches launch_check back, which checks it.
  - The round trip comes back reversed. Broken: launch_target sending the data back as it came.
  - The most data the probe found arrives whole. Broken: `xsdk_launch` dropping the last byte.
  - `argv` is as the probe found it. Broken: the command line left unsplit.
  - A launch to a `.xex` that isn't there fails with `ENOENT`, the title still running. Broken: xsdk's check left out,
    so launch_check never logs its end.
- **curl_check** (curl-xenon), against net_http.py:
  - A file over HTTP comes with its bytes right. Broken: net_http.py changing one byte.
  - HTTPS from the PC works with the test CA trusted, and `localhost` checked. Broken: `cacert.pem` trusted in its
    place, so it's refused.
  - example.com over HTTPS works with `cacert.pem`, and wrong.host.badssl.com is refused. Broken: host checking off
    (`CURLOPT_SSL_VERIFYHOST` 0).
  - A redirect is followed. Broken: following off.
  - A stalled response ends in `CURLE_OPERATION_TIMEDOUT` within a second. Broken: no timeout, so it ends only when
    net_http.py closes.
  - A refused port gives `CURLE_COULDNT_CONNECT`, and an unknown name `CURLE_COULDNT_RESOLVE_HOST`. Broken: each
    pointed at one that answers.
  - A dropped download leaves the old file as it was, and its part file. Resumed, it's whole, its SHA-256 is the
    catalogue's, and it's renamed into place. Broken: net_http.py ignoring `Range`.
  - The catalogue reads with cJSON, every entry as served, and the malformed one is refused. Broken: an entry's size
    changed in what's served.
  - The round trip: launch_target.xex downloaded to `hdd:`, its hash checked, launched, and `curl_check` launched back.
    Broken: launch_target sending nothing back.
  - The rates of step 9 are logged.
- **Needing you once, if you like:** with the cable out, `xsdk_net_link` reads no link and `xsdk_net_address` no
  address.

xsdk_test passes as before otherwise. vulkan4xenon_test twice and vkbench, unchanged. Xenia is a smoke check only:
most of this is stubs there, and it launches a title only after launch data is set.

## Done when

- curl-xenon builds through cmake/xsdk.cmake on mbedtls-xenon, and `curl_check` passes on the console, each break
  failing; HARDWARE.md records the download rates.
- A title reads the link and its address, sets socket timeouts and linger, and raises XAM's limits; xsdk_test passes
  with them, each break failing.
- A title launches another `.xex` with data and arguments, and is launched back; launch_check passes, each break
  failing.
- docs/GUIDE.md and docs/PARITY.md say what's there.

## For you to decide

1. **A curl-xenon repository** under xenosworks, as mbedtls-xenon and SDL-xenon are: curl's latest release on a
   `xenon` branch, built for HTTP and HTTPS only. Creating it is yours. Recommended: yes. The other way is a small
   HTTP/1.1 client in xsdk. It's less to carry, but every store would meet chunked bodies, redirects, kept-alive
   connections, resuming and timeouts again, all of which curl handles.
2. **The JSON library.** Recommended: cJSON. It's MIT-licensed ANSI C, one `.c` and one `.h`; it reads and writes, and
   its `malloc` is swappable through `cJSON_InitHooks`. If it builds unchanged, as step 7 checks, it needs no fork: a
   title adds its two files. The others:
   - jsmn: MIT, tokens only and no allocation, so a title converts numbers and strings itself;
   - yyjson: MIT, faster and much larger.

   C++ ports may bring their own.
3. **How launching is tested,** since a launch ends the title. Recommended:
   - launch_check and launch_target: a round trip that starts and ends in launch_check, its log the verdict, run by
     the agent with nobody there;
   - `curl_check` ending on the same round trip, with what it downloaded.

   The other way is xsdk_test's last check launching the target, which ends the run in another title. Either way,
   returning stays the launched title's own call, `xsdk_launch` back to the path the store gave it, with no format of
   xsdk's in the launch data. A title not built with xsdk goes to the dashboard when it exits, whatever xsdk does.
4. **A sample store title.** Recommended: not in this milestone. `curl_check` walks a store's path without a screen. A
   store with one, its catalogue and its downloads, would be its own repository and scope after this one.
5. **Public hosts in the checks.** Recommended: yes. example.com and badssl.com, as mbedtls-xenon's checks use, and
   one public download for the internet's rate, such as speed.cloudflare.com's. That rate is logged, not judged, as it
   depends on your line.
