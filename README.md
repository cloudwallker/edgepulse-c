# EdgePulse C

A portable C device core with deterministic fault simulation.

**Build and test an embedded acquisition pipeline without a board, existing firmware, or a virtual machine. Exercise framed communication, bounded queues, configuration recovery and watchdog resets with reproducible inputs.**

English | [中文](README_ZH.md)

[Quick start](#quick-start) · [Design](#design) · [Validation](#validation)

![Metrics from an actual default simulation](assets/demo.png)

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

`--ticks` accepts 10..1000000 one-millisecond scheduler steps; `--seed` accepts 1..4294967295. A single injected stall adds 5000 ms. Bad arguments return 2; failed reconciliation returns 1. `--help` lists options.

## Design

| Module | Implemented behavior |
| --- | --- |
| `src/protocol.c` | CRC16-CCITT-FALSE, little-endian encoding, an 82-byte stream buffer, fragmented/coalesced input and byte-wise recovery |
| `src/store.c` | Two 24-byte RAM slots, version and CRC validation, last-byte commit, interrupted writes and generation rollover |
| `src/device.c` | Cooperative sampling, sensor/link HAL callbacks, 16-frame FIFO, reject-new overload policy, retry on reconnection and reset accounting |
| `src/main.c` | Seeded sensor failures, a link outage, scheduler stall, encoded configuration commands and decoded telemetry reconciliation |

The core has no dynamic allocation. HAL callbacks run serially and must return promptly; no thread or ISR synchronization is provided. Each tick samples at most once and sends at most one queued frame. Missed sampling periods are counted instead of replayed in a burst. Unsigned millisecond arithmetic supports wraparound when adjacent calls are less than 2^31 ms apart.

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

- Sampling period: 10..60000 ms; threshold: -40000..125000 mC. Defaults: 100 ms and 30000 mC.
- A successful configuration is stored before becoming active. The last eight successful command IDs are cached per boot. Same ID and same values return duplicate without writing; different values conflict. Eviction or reboot ends this duplicate protection.
- A full queue drops the new sample and counts it. A failed link send retains the oldest frame for retry. A successful send only means transfer to the simulated link; there is no receiver ACK, durable telemetry log or exactly-once delivery guarantee.
- Config slots survive simulated watchdog resets inside one process. Starting the executable again initializes blank RAM. Flash erase is atomic only in this model; real flash timing, wear, partial erase and power-loss durability require a board backend.
- Boot IDs and counters are uint32 and wrap. Boot IDs are not persisted across process starts. Applications requiring globally unique sessions must supply a durable boot/session identity.

## Validation

```sh
python scripts/check_mutations.py --compiler gcc
```

The normal test entry point runs three C test executables and five CLI tests. Protocol tests reconcile 10000 frames and process 100000 hostile bytes; storage tests cover 50 interruption positions; device tests cover overflow, retries, sensor failure, watchdog reset, tick wrap, duplicate/conflicting commands and wire validation. The mutation test separately checks five intentionally broken variants, three runs each; compilation failures and timeouts are errors, not successful detections.

The default seed 20261023 produces 1159 successful samples: 593 delivered, 550 rejected by the full queue, 16 discarded at reset, and 0 pending. There are 40 failed sensor reads. This intentionally severe outage is a recovery test, not a loss-free throughput benchmark. See the [acceptance report](docs/验收报告.md) for commands, results and limits.

GitHub Actions defines Windows GCC and Linux GCC/ASan/UBSan checks. Local validation is recorded in the report; a workflow definition alone is not evidence that CI ran.

## Porting to a board

Replace sensor and send callbacks with ADC/I2C/SPI/UART implementations; call the device tick from a monotonic clock; feed UART/DMA chunks to the parser and invoke flush after a defined idle timeout. Replace the RAM storage model with a flash backend preserving the validated record format and commit protocol. Validate interrupt races, flash constraints, real watchdog behavior, stack limits and electrical faults on hardware. This repository does not contain an MCU startup file, linker script, BSP, RTOS or hardware validation result.
