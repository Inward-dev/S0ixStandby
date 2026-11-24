# Sleep trace: which step a hang reached

When a laptop hangs during sleep or wake and needs a forced power-off, the
evidence is usually gone: the kext's properties and `Recent log` live in
memory, and macOS writes its log in batches. `s0ixtrace` keeps one byte that
survives: the kext writes a stage code to a byte of the RTC's battery-backed
CMOS RAM at each step, and reports the last one at the next boot.

## Enable it

Add `s0ixtrace=0xNN` to the boot-args, where `0xNN` is a CMOS byte your
firmware doesn't use (see below). After the next boot, `s0ix-status.sh` shows:

```
"Sleep trace" = "CMOS 0x9c: previous boot reached 0xa4 (last step before the Deep Idle halt)"
```

## Stages

| Code | Written at | If it's the last value after a hang |
|---|---|---|
| `0xA0` | kext start | no sleep since boot (the hang wasn't in a sleep) |
| `0xA1` | sleep action starts | all drivers powered down; the hang is in S0ixStandby's sleep action |
| `0xA2` | after the `_DSM` entry calls | the hang is after the firmware's standby calls, still in the sleep action |
| `0xA3` | end of the sleep action | our sleep work finished; the hang is in XNU's remaining sleep steps |
| `0xA4` | quiesce action (last step before the CPU halts) | **the CPU entered Deep Idle and never came back**: the wake event didn't reach it, or the platform hung asleep |
| `0xA5` | active action (right after the halt) | the CPU woke; the hang is early in the wake path |
| `0xA6` | wake action starts | the wake reached our kext |
| `0xA7` | end of the wake action | `_DSM` exit and GPU reset done; the hang is in the drivers' wake (e.g. graphics) |
| `0xA8` | properties published after the wake | the wake completed; a later hang wasn't in sleep or wake |

A `Failure during sleep: GFX0()` hang should leave `0xA0`, `0xA7` or `0xA8`
(the framebuffer hangs before our sleep action runs, so the byte still holds
the end of the previous cycle). An "EFI/Bootrom Failure after
last point of entry to sleep" hang should leave `0xA1` or later.

## Choosing a CMOS byte

The kext refuses bytes below `0x38` (RTC clock and status registers and the
standard checksum area). Bytes `0x38`–`0x7F` are in the standard bank (ports
0x70/0x71), `0x80`–`0xFF` in the extended bank (0x72/0x73).

Firmware uses CMOS RAM for its own settings, and some firmware checksums the
bytes it uses. Writing a byte the firmware owns can reset BIOS settings or
show a "CMOS checksum" message at the next boot. To pick a byte:

1. Prefer the **extended bank** (`0x80`–`0xFF`), and stay away from ranges
   your OpenCore `RTCMemoryFixup` excludes (`rtcfx_exclude=...`): those are
   bytes you already know macOS or the firmware write.
2. If you can boot Linux, `sudo hexdump -C /dev/nvram` shows the standard
   bank from `0x0E` on. A byte that reads `00` and stays `00` across reboots is
   a good candidate there.
3. After enabling it, do **one normal reboot** before any sleep test. If the
   BIOS complains or your settings changed, remove the boot-arg, load the BIOS
   defaults once, and try another byte.

Remove the boot-arg when you're done. The byte keeps its last value; that's
harmless if the firmware doesn't use it.
