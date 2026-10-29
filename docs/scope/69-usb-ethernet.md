# Milestone 69: a USB Ethernet adapter on the Xbox 360

Agreed on 2026-10-28. The user wants FTP faster than the built-in 100 Mbit NIC allows, through a USB
Ethernet adapter, in the end as a plugin that DashLaunch loads, so FTP servers and titles use it.

## Why

- FTP to the console runs at about 8 MB/s; the NIC's ceiling is about 11 MB/s.
- Measured on the console (local/hdd_write, 2026-10-28): the internal drive takes 25-43 MB/s of sequential writes, and
  the kernel's USB mass-storage path reads about 20 MB/s on two drives of different speeds. So an adapter on the
  console's USB 2.0 should reach about 15-20 MB/s: two to two and a half times today's FTP, not 40.

## Decisions (the user, 2026-10-28)

1. **Bring-up in a standard xsdk title,** which runs in kernel mode on the console, until the adapter is registered
   and has a link and moves frames; then the working code goes into a plugin. Plugin loading and the driver aren't
   debugged at the same time.
2. **The adapter:** the user's Realtek RTL8156, `0bda:8156`.

## Sources

- **The kernel's USB stack,** retail kernel 17559: Jacckii/Xbox360-Series-Controllers-Native, `docs/INTERNALS.md`,
  public reverse engineering, cited as `xcn:docs/INTERNALS.md`. It gives the runtime class-driver list
  (`0x801A8048`, walked by `UsbdSelectClassDriver`, matched by VID/PID), the Usbd exports by ordinal (740
  `UsbdAddDeviceComplete`, 741 cancel, 746/747 open endpoints, 748 `UsbdQueueAsyncTransfer`, 749/750 close, 751
  `UsbdRemoveDeviceComplete`, 759 `UsbdGetDeviceDescriptor`, 890 rescan parked ports), and the rules: completions on
  Usbd's DPC, Usbd takes no locks so requests go through a ring its DPC runs, no floating point in DPC code, IN
  buffers page-aligned within one EHCI descriptor (0x5000 bytes from the page), a control transfer with no data stage
  takes a NULL buffer. That project builds with OpenXeChain, which we don't use; we take its facts, not its code.
- **The adapter:** the USB-IF's CDC NCM 1.0 and CDC 1.2 specifications for the standard path. Linux's
  `drivers/net/usb/r8152.c` and `cdc_ncm.c` (GPL-2.0) as references only, cited as `linux:<path>:<line>`, kept in
  `local/usbnet/ref`; no code taken from them. `r8152.c` lists `0x8156` (line 10382) and picks the adapter's
  vendor-specific configuration, noting it "is not always config #1" (line 10446): the adapter has more than one.
- **Kernel exports** as Xenia names them (xenos-toolchain/reference, as in SCOPE.md).

## Steps

1. **Find the adapter and dump its descriptors** (title). Register a class-driver entry for `0bda:8156`, take the
   device when the kernel offers it, open the default endpoint, and log the device descriptor and every configuration
   descriptor with its interfaces, alternate settings and endpoints. The title unlinks its entry, cancels its transfers
   and closes its endpoints before it exits, also when the console agent ends it, since the kernel would otherwise call
   into an unmapped title. It refuses to run on any kernel but 17559.
   Check: the descriptors in the log; a second run, and unplugging and replugging after the title exits, don't crash
   the console.
2. **Link up** (title). If the adapter offers a CDC NCM configuration, select it (`SET_CONFIGURATION`, then the data
   interface's alternate setting), read the MAC address from its functional descriptor, read the NTB parameters, and
   open the bulk and interrupt endpoints; link state and speed come as the interrupt endpoint's notifications. If it
   offers only the vendor mode, stop and agree a new step: Realtek's own protocol is far larger (`r8152.c` is over
   10,000 lines of register and firmware setup).
   Check: link up with its speed in the log; pulling the cable logs link down.
3. **Frames** (title). Send and receive NCM transfer blocks; answer ARP and ICMP echo for a fixed address, so the PC
   pings the console through the adapter; then a raw UDP throughput test against a PC tool, in MB/s.
   Check: replies to the PC's ping; the throughput, against the NIC's 11 MB/s.
4. **Plugins from xsdk.** Build a system DLL that DashLaunch loads from `launch.ini`: its entry, DPC code without
   floating point, and thunks that keep the kernel's registers as `xcn:docs/INTERNALS.md` describes. A plugin that
   only logs first.
   Check: it loads at boot, logs, and stays across title changes. This also lets the console agent leave the XDK.
5. **The driver as a plugin.** Steps 1-3's code, now staying registered across titles.
   Check: steps 1-3's checks again, across a title change.
6. **The kernel's network stack.** Find out, by reverse engineering, how XNet attaches a network interface (the
   built-in NIC and the official wireless adapter, which is a USB device), and attach the adapter, so FTP servers and
   titles' sockets use it.
   Check: FTP to the console through the adapter, above 8 MB/s.

## Open

- Whether step 6 is possible; it decides how much the milestone is worth. Steps 1-3 tell whether the hardware path is.
- Where the code lives: in xsdk (a library and the plugin), or its own repository.
- How the adapter and the built-in NIC share addresses and routes once both are up.

## Left out

Xbox Live, Wi-Fi adapters, and other adapters' vendor protocols. An NCM driver works for any NCM adapter without
more work.
