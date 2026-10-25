# EdgePulse C

A portable C device core with deterministic fault simulation and persistent host configuration.

**Build and test an embedded acquisition pipeline without a board, existing firmware, or a virtual machine. Exercise framed communication, bounded queues, configuration recovery across processes and watchdog resets with reproducible inputs.**

English | [中文](README_ZH.md)

[Quick start](#quick-start) · [Design](#design) · [Validation](#validation)

![Actual persistent configuration commands and demo output](assets/demo.png)

The image lays out actual CLI output; it is not a terminal screenshot.

## Quick start

Requires a C11 compiler. Tests also use Python 3.9+ (standard library only). The application and its core are written in C and have no third-party runtime dependencies.

Windows with GCC on PATH:

```powershell
./scripts/build.ps1 -Mode test
./build/edgepulse.exe --demo
```

If GCC or Python is not on PATH, pass `-Compiler` and `-Python` with their executable paths. MSYS2 GCC is supported; no virtual machine is needed.

Linux:

```sh
make test
./build/edgepulse --demo
make sanitize
```

Run another reproducible scenario:

```powershell
./build/edgepulse.exe --seed 42 --ticks 12000 --json
```

Save configuration, read it in another process, and run a demo with those values:

```powershell
./build/edgepulse.exe config set build/device.epcfg 25 -12500
./build/edgepulse.exe config show build/device.epcfg
./build/edgepulse.exe --store build/device.epcfg --demo --json
./build/edgepulse.exe config show build/device.epcfg
```

On Linux, use `./build/edgepulse` with the same arguments. `set` creates a missing file or updates a valid existing file; `show` and `--store` require an existing valid file. Parent directories must already exist. Paths containing spaces must be quoted; the Windows CLI and backend support Unicode paths. Local `*.epcfg` files are ignored by Git.

`config set FILE PERIOD_MS THRESHOLD_MC` accepts a period of 10..60000 ms and a threshold of -40000..125000 mC. `set` and `show` print JSON containing `period_ms`, `threshold_mc`, `generation` and `persistent:true`. `show` does not create or write a file. The demo holds the file lock until exit and submits the loaded values once through the configuration protocol, then sends a duplicate command: values stay the same, while the successful submission advances the stored generation once. Demo JSON includes the active values and `persistent` status.

`--ticks` accepts 10..1000000 one-millisecond scheduler steps; `--seed` accepts 1..4294967295. A single injected stall adds 5000 ms. Exit status is 0 on success, 2 for invalid arguments, and 1 for storage, locking, format or reconciliation errors. Storage errors produce no success JSON. `--help` lists options.

## Design

| Module | Implemented behavior |
| --- | --- |
| `src/protocol.c` | CRC16-CCITT-FALSE, little-endian encoding, an 82-byte stream buffer, fragmented/coalesced input and byte-wise recovery |
| `src/store.c` | Two 24-byte records, version and CRC validation, last-byte commit, RAM interruption tests, generation rollover and an optional commit callback |
| `src/file_store.c` | A 48-byte host configuration file, exclusive sessions, three synchronized commit stages and recovery from the remaining valid slot |
| `src/device.c` | Cooperative sampling, sensor/link HAL callbacks, 16-frame FIFO, reject-new overload policy, retry on reconnection and reset accounting |
| `src/main.c` | Configuration set/show commands, optional file-backed demos, seeded sensor failures, a link outage, scheduler stall and decoded telemetry reconciliation |

The protocol, store and device core have no dynamic allocation or OS dependencies. The host CLI and file backend may allocate memory for Windows Unicode conversion. HAL callbacks run serially and must return promptly; no thread or ISR synchronization is provided. Each tick samples at most once and sends at most one queued frame. Missed sampling periods are counted instead of replayed in a burst. Unsigned millisecond arithmetic supports wraparound when adjacent calls are less than 2^31 ms apart.

The threshold produces a telemetry alarm flag. There is no physical actuator driver. A simulated watchdog detects a scheduler gap of at least 5000 ms on the next tick, increments the boot identifier, accounts for discarded queued frames, clears command history and reloads configuration.

### Wire format

All multibyte fields use little endian. Maximum payload length is 64 bytes.

| Byte offset | Field |
| --- | --- |
| 0..1 | Sync `A5 5A` |
| 2..3 | Version `01`, message type |
| 4..5 | Device ID, uint16 |
| 6..9 | Boot ID, uint32 |
| 10..13 | Sequence / command ID, uint32 |
| 14..15 | Payload length, uint16 |
| 16.. | Payload, followed by uint16 CRC |

CRC covers version through payload; polynomial 0x1021, initial 0xFFFF, no reflection or final XOR. `123456789` produces 0x29B1. Type 1 telemetry is `timestamp:u32 + temperature_mC:i32 + alarm:u8`. Type 2 configuration is `period_ms:u32 + threshold_mC:i32` and must match the current device and boot ID.

The parser accepts arbitrary transport chunks. On receiver idle/session end, call `ep_parser_flush` to reject an incomplete candidate and recover any complete frames behind it. The parser does not own a clock or enforce an idle timeout itself. CRC detects corruption; it does not authenticate commands.

### Configuration and delivery semantics

- Sampling period: 10..60000 ms; threshold: -40000..125000 mC. Core defaults and new file initialization: 100 ms and 30000 mC. The default RAM demo applies 10 ms and 30000 mC; a `--store` demo uses the saved values.
- A successful configuration is stored before becoming active. The last eight successful command IDs are cached per boot. Same ID and same values return duplicate without writing; different values conflict. Eviction or reboot ends this duplicate protection.
- A full queue drops the new sample and counts it. A failed link send retains the oldest frame for retry. A successful send only means transfer to the simulated link; there is no receiver ACK, durable telemetry log or exactly-once delivery guarantee.
- Without `--store`, config slots survive simulated watchdog resets inside one process and are reset to blank RAM on the next process start. The optional file backend preserves configuration across process starts. Its host file protocol does not implement MCU flash erase, timing or wear behavior.
- Boot IDs and counters are uint32 and wrap. Boot IDs are not persisted across process starts. Applications requiring globally unique sessions must supply a durable boot/session identity.

### Persistent configuration

The file contains exactly two 24-byte version-1 records, each with a generation, configuration, CRC and final commit byte. Existing files must have the exact 48-byte size and at least one valid record. Unknown, truncated or doubly corrupt files are rejected without rewriting them; a single valid slot remains recoverable. Creating a file uses exclusive creation and writes a synchronized default record before the requested update. Interrupted initialization can leave an invalid file; later opens reject it without overwriting it.

Each open session holds an exclusive lock: `CreateFileW` with no sharing on Windows, or nonblocking `flock` on Linux. Concurrent EdgePulse sessions fail promptly; closing or terminating the owner releases the lock. `show` has read-only configuration semantics, but uses the same exclusive read/write handle and therefore still needs file access permission. Only local regular files are supported; directories and final-component symlinks/reparse points are rejected. The backend does not traverse parent directory components to check for links.

An update touches only the inactive slot, in this order:

1. Clear its commit byte and synchronize the file.
2. Write bytes 0..22, including the CRC, and synchronize the file.
3. Write the final commit byte and synchronize the file.

Synchronization uses `FlushFileBuffers` on Windows and `fsync` on Linux. Only a confirmed backend commit updates the RAM image, active device configuration and successful-command history. An I/O or synchronization error leaves the outcome unconfirmed: after a final synchronization failure, reopening may reveal the complete new configuration. The backend refuses further writes until closed and reopened. `store_writes` counts accepted write attempts, including failed attempts; it is not a durable-success counter. RAM-only partial-write injection remains available, while file-backed saves require a complete-record operation.

The process-termination tests check recovery at explicit write/synchronization boundaries. They do not establish physical power-loss durability, network-filesystem behavior, sector independence or durability of the directory entry when a file is first created.

## Validation

```sh
python scripts/check_mutations.py --compiler gcc
```

The normal test entry point runs three C test executables, nine CLI tests and 14 file-backend process tests. Protocol tests reconcile 10000 frames and process 100000 hostile bytes; storage tests cover 50 RAM interruption positions plus the external commit boundary; device tests cover overflow, retries, sensor failure, watchdog reset, tick wrap, duplicate/conflicting commands, wire validation and commit failure. File tests include 27 real process terminations at commit boundaries and 28 injected write/synchronization failures, alongside locking, format and access checks. The mutation test separately checks five intentionally broken variants, three runs each; compilation failures and timeouts are errors, not successful detections.

The default RAM demo, without `--store`, preserves the compatibility baseline: seed 20261023 produces 1159 successful samples, with 593 delivered, 550 rejected by the full queue, 16 discarded at reset, and 0 pending. There are 40 failed sensor reads. With the saved 25 ms / -12500 mC configuration above, the same seed and tick count produce 463 successful samples: 246 delivered, 201 dropped, 16 discarded at reset, and 0 pending. This intentionally severe outage is a recovery test, not a loss-free throughput benchmark. See the [acceptance report](docs/验收报告.md) for commands, verified results and limits.

On 2026-10-25, Windows GCC 13.2.0 validation passed through both the PowerShell entry point and Make from an empty build directory: protocol 8/8, store 10/10, device tests, CLI 9/9, and 13 of 14 file tests with the POSIX-only FIFO test skipped. Windows symlink rejection was exercised successfully. All 15 mutation runs detected their defects. Linux and ASan/UBSan were not run locally; their results are established by the PR's GitHub Actions checks, not by the workflow definition alone.

## Porting to a board

Replace sensor and send callbacks with ADC/I2C/SPI/UART implementations; call the device tick from a monotonic clock; feed UART/DMA chunks to the parser and invoke flush after a defined idle timeout. Attach a board flash backend at the store commit boundary and adapt the record format and commit protocol to its erase/program constraints; the host file backend is not a board driver. Validate interrupt races, flash constraints, real watchdog behavior, stack limits and electrical faults on hardware. This repository does not contain an MCU startup file, linker script, BSP, RTOS or hardware validation result.
