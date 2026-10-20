# moldwatch

A bathroom mold-risk monitor. It sits on a shelf, touches nothing, and
answers one question its user cannot answer today: how many hours a day
does the bathroom ceiling stay wet enough for mold, and is the exhaust
fan actually doing anything about it.

See PROBLEM.md for the user and the problem. DESIGN.md (M2, Week 7)
will carry the architecture.

## Status

Milestone M0/M1. Hardware ordered; no product code yet by design.

## Hardware

| Part | Interface | Role |
|---|---|---|
| Raspberry Pi (40-pin header) | -- | host |
| BME280 | I2C (0x76/0x77) | room air temperature + relative humidity |
| MLX90614 | I2C (0x5A) | non-contact ceiling surface temperature |

Both sensors are 3.3V and share one I2C bus at different addresses.
Wire with the board powered off; check voltages with a meter first.

## Build

    make            # build everything
    make test       # unit tests
    make asan       # sanitizer build
    make soakcheck  # 1-hour miniature of the 48h soak

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
