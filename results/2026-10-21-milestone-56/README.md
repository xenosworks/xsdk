# Milestone 56 step 1: the probes (2026-10-21)

probe_net, probe_launch and probe_launch_t2, built from `9545128` (branch M56, rebased onto main `ffbd8f5`), run on the
console by the agent against tools/net_echo.py, as local/m56/probes' README set out
(docs/scope/56-store-networking.md, step 1). docs/HARDWARE.md, "XAM's sockets and loader, milestone 56", says what
they found.

- `net_1_defaults.log` to `net_4_restart.log`: probe_net, one run a start file. The restart, which might have hung the
  title, ran last.
- `probe_launch_record.txt`: every launch run's lines, both titles', in order. The device-path launch's start file read
  `path device`, the USB one's `path usb`; with the device path not taken, the data, GPU-closed and GPU-held cases
  launched by `game:` instead of `device`. Six lines at 08:25:43 are left out: that run was started by a tool this
  project doesn't cite.
- `launch_device.log`, `launch_usb.log`, `launch_missing.log`: probe_launch's own log of the launches that never
  arrived.
- `launch_aurora.log`: the look case, launched by hand from Aurora with the controller.
