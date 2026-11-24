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

## Safety checks

- **The kext only writes an empty byte.** If the byte holds anything other
  than `0x00` or one of the stage codes, it is probably firmware data, and the
  kext refuses (`Sleep trace = refused: CMOS 0x.. holds 0x.., probably used by
  the firmware`). `-s0ixtraceforce` overrides that; don't use it unless you
  know the byte is free.
- **`CMOS bytes reading 0x00`** lists the extended-bank bytes (`0x80`–`0xFF`)
  that read `0x00` (read-only scan, done whenever `s0ixtrace` is set). Pick one
  from it.
- **Restoring a byte:** `s0ixtrace=0xNN s0ixtraceset=0xVV` writes `0xVV` to
  byte `0xNN` once and leaves tracing off. Use it if a byte you tried held
  something else (the old value is in `Sleep trace`), then remove both
  boot-args.

## Checking that the byte works

At start the kext writes `0xA0` and reads it straight back. `Sleep trace` ends
in `write check ok`, or `write check FAILED (wrote 0xa0, read 0x..)`. A failed
check means the chipset ignores writes there: on Intel chipsets the firmware
can disable or lock the extended bank. Then the trace can't be used safely on
that machine.

If the check is ok but every boot still reports `no trace from the previous
boot (byte was 0x00)`, the firmware clears that byte at power-on: try another
one. `Sleep trace now` shows the byte's value after the last wake.

## Choosing a CMOS byte

Only the **extended bank** (`0x80`–`0xFF`, ports 0x72/0x73) is accepted by
default. The standard bank (`0x0E`–`0x7F`) is where firmware keeps its own
checksummed settings: on an HP EliteBook 840 G8, writing byte `0x60` caused a
"CMOS checksum" error at the next boot and the BIOS reset its settings to
defaults. That happens whatever the byte held before, so "reads `0x00`" does
not make a standard-bank byte safe. `-s0ixtraceforce` allows `0x38`–`0x7F` and
in-use bytes; avoid it.

Firmware uses CMOS RAM for its own settings, and some firmware checksums the
bytes it uses. Writing a byte the firmware owns can reset BIOS settings or
show a "CMOS checksum" message at the next boot. To pick a byte:

1. Start from `CMOS bytes reading 0x00`, and stay away from ranges
   your OpenCore `RTCMemoryFixup` excludes (`rtcfx_exclude=...`): those are
   bytes you already know macOS or the firmware write.
2. After enabling it, do **one normal reboot** before any sleep test. If the
   BIOS complains or your settings changed, remove the boot-arg, load the BIOS
   defaults once, and try another byte.

Remove the boot-arg when you're done. The byte keeps its last value; that's
harmless if the firmware doesn't use it.
