# moldwatch: team rules

Bathroom mold-risk monitor. BME280 (room air) + MLX90614 (non-contact
ceiling surface temp), fused into surface relative humidity, logged for
weeks, on a Raspberry Pi.

TEAM: Ricky Pena, Luke Sellers

## Commands
- Build: `make`          Cross/deploy: `make deploy PI=pi@<host>`
- Tests: `make test`     Sanitizers: `make asan`   Valgrind: `make memcheck`
- Soak sanity: `make soakcheck` (1-hour miniature of the 48h run)
- A change is DONE only when build, test, and asan pass. Show output.

## Hard constraints
- The PRODUCT makes no network calls except serving its own LAN
  interface. No LLM APIs, no cloud inference, no pretrained models.
  The intelligence in analysis/ is ours. This is a graded boundary.
- Dew point, surface RH, wet-hours accounting, the shower-event state
  machine, and the drying-time trend are OUR code. No library does the
  deciding. Math primitives are fine if we can explain them.
- Every daemon must be supervisable: clean exit codes, no orphaned
  fds across restart, heartbeat within 60s of start.
- Every allocation checked; every syscall's error path handled and
  logged. The 48-hour soak is the test suite of last resort.
- A sensor that disappears is the expected case, not the exception.
  Losing the MLX90614 must degrade the product, never stop it.
- NEVER weaken, skip, or delete a test to make the suite pass.

## Ownership
- Ricky Pena/: capture pipeline, kernel-facing code (src/capture)
- Luke Sellers/: storage layer, analysis engine (src/store, src/analysis)
- Shared: supervisor, interface. The agent edits outside the current
  session owner's area only when told explicitly whose session this is.

## Style
- Systems core: C17, -Wall -Wextra -Werror, no VLAs.
  goto-cleanup for multi-resource functions.
- Python permitted only under tools/ and ui/. No graded mechanism
  may live there.
- Smallest diff that passes. Do not refactor unrelated code.

## Workflow
- Multi-file or algorithmic change: plan first, wait for approval.
- Hardware bugs: paste real evidence (i2cdetect, dmesg, timing capture,
  /proc/interrupts). No fixes proposed from a verbal description.
- Two I2C devices share one bus. Any timing or contention claim needs a
  capture, not an assertion.
- Commit only from a green state; message format "M<n>: <what>".
