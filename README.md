# moldwatch

A bathroom mold-risk monitor. It sits on a shelf, touches nothing, and
answers one question its user cannot answer today: how many hours a day
does the bathroom ceiling stay wet enough for mold, and is the exhaust
fan actually doing anything about it.

See PROBLEM.md for the user and the problem. DESIGN.md (M2, Week 7)
will carry the architecture.

## Status

M3 in progress. Capture runs end to end on synthetic data; the BME280
and MLX90614 readouts are next. Storage (mechanism D) is in progress.

## Hardware

| Part | Interface | Role |
|---|---|---|
| Raspberry Pi (40-pin header) | -- | host |
| BME280 | I2C (0x76/0x77) | room air temperature + relative humidity |
| MLX90614 | I2C (0x5A) | non-contact ceiling surface temperature |

Both sensors are 3.3V and share one I2C bus at different addresses.
Wire with the board powered off; check voltages with a meter first.

## Build

    make            # build everything into build/
    make test       # unit tests
    make asan       # build and test under AddressSanitizer + UBSan
    make memcheck   # tests and a fake capture under valgrind

## Run capture

One process per sensor. Records go to stdout; the timestamped log
(startup, heartbeats, sensor lost/recovered, shutdown) goes to stderr.

    ./build/capture --sensor bme280                 # live, on the Pi
    ./build/capture --sensor mlx90614
    ./build/capture --sensor bme280 --binary        # raw records for storage

Without hardware, `--fake` produces synthetic readings. Every fake record
carries the REC_SYNTHETIC flag and is printed with SYNTHETIC, and the log
says mode=SYNTHETIC. `--fake-drop START:LEN` simulates the sensor failing
for LEN samples, to rehearse fault handling:

    ./build/capture --sensor bme280 --fake --interval-ms 500 --fake-drop 10:5

## Network

The product makes no network calls. Nothing under src/ opens a socket.

## Layout

    src/capture/     sensor sampling processes
    src/store/       append-only crash-consistent log (mechanism D)
    src/analysis/    dew point, surface RH, wet hours, shower detection
    src/supervisor/  process supervision and restart (mechanism E)
    src/ui/          CLI query interface
    tools/           dev tooling, replay harness
    tests/           unit tests
    docs/            design notes
    transcripts/     per-partner raw Claude Code .jsonl files
