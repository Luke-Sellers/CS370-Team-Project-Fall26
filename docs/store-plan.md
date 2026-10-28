# Plan: storage layer (src/store), M3, revision 2

## Context
Capture already emits 32-byte `struct record`s when run with `--binary`
(`capture.c:182,199`). Nothing persists them yet. PROBLEM.md: "a bathroom outlet
losing power mid-write must not corrupt or erase" weeks of history.

Fixed decisions:
- fsync every record
- segments per sensor: a segment never spans a boot and holds at most
  **8640 records** (24 h at the default 10 s interval); the next append goes to
  counter+1; versioned header with boot id
- recovery truncates a torn tail
- mid-file corruption is never resynced: the segment is quarantined (renamed,
  every byte kept) and a fresh segment starts
- one storage process per sensor reading a pipe
- CRC lives in src/store
- record.h and src/capture untouched
- Linux is where tests count
- Makefile changes are additive only
- the work lands as six steps, each with its own tests and an `M3:` commit

---

## 1. On-disk format v1

### Directory (`--dir`, must exist)
```
bme280.lock                    flock target, never deleted, empty
bme280.000001.mwlog            sealed segment (an earlier boot)
bme280.000002.mwlog.corrupt-1  quarantined segment, bytes untouched
bme280.000003.mwlog            live segment (this boot)
mlx90614.lock / mlx90614.000001.mwlog ...
```
- The counter is `%06u`. It is parsed numerically, never sorted as a string.
  Names that don't match the pattern are ignored.
- **Next counter = 1 + the maximum counter over live and quarantined names.** A
  counter is never reused, so counter order is creation order.
- Quarantine name: `<segment>.corrupt-<n>`, where n is the smallest unused value
  ≥ 1. Re-quarantining never overwrites.
- **Segment rule:** append to the live segment only if it belongs to this boot
  **and** holds fewer than `STORE_SEG_MAX` (8640) records. Otherwise create
  counter+1 and append there.
  - The live segment always has the highest counter of any file, because we hold
    the lock and every segment we create is `max_any+1`. So counter+1 is the same
    as `max_any+1`.
  - A new segment is fully created (tmp, fdatasync, rename, dir fsync) before the
    record is written into it. A crash in between leaves a full old segment and at
    most a stale `.tmp`, which startup handles.

### Segment layout
A 32-byte header, then N × 32-byte `struct record`. Records sit on 32-byte
boundaries, and `(size-32) % 32` is the partial-tail byte count.

### Header (`struct store_header`, `_Static_assert` == 32)
| off | type | field | value |
|---|---|---|---|
| 0 | uint32 | magic | `0x474C574D` ("MWLG" on disk) |
| 4 | uint16 | version | 1 |
| 6 | uint8 | sensor | `enum sensor_id` |
| 7 | uint8 | record_size | 32 |
| 8 | uint32 | counter | must equal the counter in the filename |
| 12 | uint8[16] | boot_id | from `/proc/sys/kernel/random/boot_id`; all zeros if unreadable (logged) |
| 28 | uint32 | crc32 | over bytes [0,28) |

### Records
Records are exactly record.h's struct. Storage fills `crc32` over bytes [0,28).
Every other bit is stored as received: SYNTHETIC, READ_ERR, seq, t_ns, a, b,
reserved. Order within a segment, then by counter, is arrival order. Capture
restarts within a boot show up as `seq` dropping.

### CRC
CRC-32/IEEE: reflected, polynomial 0xEDB88320, init and xorout 0xFFFFFFFF,
bitwise implementation. Check value `0xCBF43926`.

### Creating a segment
1. Write the header to `<seg>.tmp` (O_CREAT|O_EXCL|O_CLOEXEC).
2. `fdatasync` it.
3. `rename` it to `<seg>`.
4. `fsync` the dir.

### Appending
1. `write` 32 bytes (O_APPEND).
2. `fdatasync`.

If the write is short or fails, or fsync fails: `ftruncate` to the last good size
and return an error. The caller exits; there is no retry.

### Quarantine
1. `rename(<seg>, <seg>.corrupt-<n>)`.
2. `fsync` the dir.

This is race-free because the per-sensor lock file is held.

---

## 2. API: `src/store/store.h`
```c
#include <stddef.h>
#include <stdint.h>
#include "record.h"

#define STORE_MAGIC   0x474C574Du
#define STORE_VERSION 1u
#define STORE_SEG_MAX 8640u    /* records per segment, then rotate to counter+1 */

struct store_header { uint32_t magic; uint16_t version; uint8_t sensor; uint8_t record_size;
                      uint32_t counter; uint8_t boot_id[16]; uint32_t crc32; };
_Static_assert(sizeof(struct store_header) == 32, "header must stay 32 bytes");

enum store_status {
    STORE_OK = 0,
    STORE_EIO      = -1,  /* syscall failed; errno preserved, logged */
    STORE_EBADHDR  = -2,  /* magic/crc/record_size/counter mismatch/short file */
    STORE_EVERSION = -3,  /* refuse: not ours to touch */
    STORE_ESENSOR  = -4,  /* refuse: header sensor != filename sensor */
    STORE_ECORRUPT = -5,  /* bad record before the tail */
    STORE_EBUSY    = -6,  /* another writer holds <sensor>.lock */
    STORE_EINVAL   = -7,  /* record rejected (magic/sensor/reserved) */
};

struct store_open_info {
    uint32_t counter;            /* segment now being appended to */
    int      created;            /* 1 = new segment this open */
    int      new_boot;           /* 1 = previous live segment belonged to another boot */
    uint64_t records;            /* records already in the live segment */
    uint64_t truncated_bytes;    /* torn tail removed from the previous latest segment */
    int      quarantined;        /* 1 = latest segment was renamed .corrupt-<n> */
    uint32_t quarantined_counter;
    uint64_t bad_offset;         /* why: offset of first bad byte/record */
    uint32_t last_seq;           /* last valid record of latest segment, 0 if none */
    int64_t  last_t_ns;
};

uint32_t store_crc32(const void *buf, size_t len);                     /* step 1 */
int store_header_check(const struct store_header *h, enum sensor_id s,
                       uint32_t counter);                              /* step 2 */
int store_record_check(const struct record *r, enum sensor_id s);      /* magic, sensor, crc */

struct store;
int store_open(const char *dir, enum sensor_id s, struct store **out,
               struct store_open_info *info);                          /* steps 2 and 4 */
int store_append(struct store *st, const struct record *r);            /* step 3; rotates at STORE_SEG_MAX */
uint32_t store_counter(const struct store *st);                        /* live segment, for heartbeat/tests */
int store_close(struct store *st);

/* Reader (step 5). Snapshots the file list at open and walks it in order
 * (counter ascending; for one counter: live file, then .corrupt-1, .corrupt-2...).
 * Read-only: takes no lock, never truncates or renames.
 *   Live/sealed segment: bad header -> skip; partial/bad last record -> end of
 *     segment; bad record mid-segment -> stop that segment, count, move on.
 *   Quarantined file: valid header -> return records up to the first bad or
 *     partial record (salvaged), then move on; bad header -> 0 salvaged, count it. */
struct store_reader;
struct store_rec_src { uint32_t counter; uint8_t boot_id[16]; int quarantined; };
struct store_reader_stats {
    uint32_t segments;            /* live/sealed files read */
    uint32_t bad_header;          /* files skipped for a bad header (either kind) */
    uint32_t corrupt_stops;       /* live/sealed segments cut short mid-file */
    uint32_t quarantined_files;   /* .corrupt-* files visited */
    uint64_t salvaged_records;    /* records returned from .corrupt-* files */
};
int store_reader_open(const char *dir, enum sensor_id s, struct store_reader **out);
int store_reader_next(struct store_reader *rd, struct record *out,
                      struct store_rec_src *src);                       /* 1 rec, 0 end, <0 EIO */
void store_reader_stats(const struct store_reader *rd, struct store_reader_stats *out);
int store_reader_close(struct store_reader *rd);
```

### Daemon `build/store` (`src/store/stored.c`, step 6)
`store --sensor bme280|mlx90614 --dir DIR [--heartbeat-s N]`, reading stdin.

- **Startup:** log the open_info summary. Quarantine and truncation are logged loudly.
- **Loop:** `poll` with a heartbeat timeout (≤ 60 s, first one at start). Assemble
  32-byte records, validate the input, then `store_append`.
- **Heartbeat fields:** counter, records, last_seq, max fsync ms, rss.
- **Exit codes:**
  - 0: EOF or signal. A partial record is logged and dropped.
  - 1: usage.
  - 3: write/fsync failed.
  - 4: setup.
  - 5: EVERSION / ESENSOR / EBUSY. Corruption no longer exits; it quarantines.
  - 6: input framing (bad magic or wrong sensor, e.g. capture not run with `--binary`).
- Reuses `src/capture/log.c` unmodified.

---

## 3. Recovery / segment selection (inside `store_open`)
```
1. open <sensor>.lock O_CREAT|O_RDWR|O_CLOEXEC; flock(LOCK_EX|LOCK_NB) -> EBUSY
2. read current boot_id (zeros + log if unreadable)
3. scan dir: unlink stale <sensor>.<N>.mwlog.tmp (log each)
   max_live = highest N with <sensor>.<N>.mwlog
   max_any  = highest N over live and .corrupt-* names
4. no live segment -> create max_any+1 (or 1) -> done
5. open latest = max_live O_RDWR|O_APPEND|O_CLOEXEC
   size < 32 or header bad (magic/crc/record_size/counter) -> CORRUPT path
   version != 1 -> EVERSION (refuse, no change)
   sensor  != s -> ESENSOR  (refuse, no change)
6. n = (size-32)/32; scan records 0..n-1 (4 KiB preads); i = first invalid
     none   -> valid = n
     i==n-1 -> valid = n-1          (torn last full record)
     i< n-1 -> CORRUPT path
   if 32+32*valid != size: ftruncate; fdatasync; log bytes dropped
7. if boot_id == current and current != zeros and valid < STORE_SEG_MAX:
       append to latest -> done
   else: latest stays sealed (tail already clean); create max_any+1 -> done
   (a segment holding more than STORE_SEG_MAX valid records is not an error:
    sealed as-is, logged)

store_append, before writing:
   if records_in_live == STORE_SEG_MAX:
       create max_any+1 (== counter+1) with the current boot_id; fdatasync, rename,
       dir fsync; close the old fd; log "rotated to N"
       on failure -> STORE_EIO, nothing appended, old segment untouched
CORRUPT path: close latest; rename to <seg>.corrupt-<k>; fsync dir; log
   offset and reason; create max_any+1; info.quarantined = 1 -> done (STORE_OK)
```
- Only the latest segment is scanned at startup. Sealed segments were clean when
  they were sealed, so the scan cost is bounded to one boot's worth of data.
- With fsync per record, at most one record is ever in flight. So the only
  legitimate damage is a partial tail, or one complete-length bad last record.
  Anything else is corruption.

---

## 4. Implementation steps
Each step runs:
1. Write its tests.
2. Show `make test` failing.
3. Implement.
4. Show `make`, `make test`, and `make asan` green (plus `make memcheck` from step 3 on).
5. Commit.

Every Makefile change below is a line **added**. No existing line is edited or deleted.

### Step 0: commit this plan
- Copy this plan verbatim to `docs/store-plan.md` (the only new file; `docs/.gitkeep` stays).
- CLAUDE.md says commit only from green, so first run `make && make test && make asan`
  and show the output. If this Mac can't build capture (`clock_nanosleep` and
  `/proc` are Linux-only), stop and report that rather than committing.
- Commit only `docs/store-plan.md`: `M3: storage layer plan` with the Co-Authored-By trailer.
- Every edit is approved manually by Luke; there are no auto-accepted edits.

### Step 1: CRC
- Files: `src/store/crc32.c`, `src/store/crc32.h`, `tests/test_crc.c`.
- Makefile additions:
  ```make
  STORE_HDR := include/record.h $(wildcard src/store/*.h)
  $(BUILD)/test_crc: tests/test_crc.c src/store/crc32.c $(STORE_HDR)
  	$(CC) $(CPPFLAGS) -Isrc/store $(CFLAGS) -o $@ tests/test_crc.c src/store/crc32.c
  .PHONY: test-store-crc
  test: test-store-crc
  test-store-crc: $(BUILD)/test_crc
  	./$(BUILD)/test_crc
  ```
  An extra prerequisite-only `test:` line makes the existing `test` and `asan`
  pick it up without touching them. Make forbids a second recipe for one target,
  so every later step follows the same pattern: its own phony
  `test-store-<step>` target with its recipe, plus a `test: test-store-<step>` line.
- Tests:
  - C1. "123456789" gives 0xCBF43926.
  - C2. Empty input gives 0.
  - C3. 32 zero bytes give a precomputed constant.
  - C4. Computing in pieces equals computing in one shot (if an incremental form is exposed; otherwise drop C4).
- Commit: `M3: store CRC-32 with check-value tests`.

### Step 2: create, header, segment selection
- Files: `src/store/store.h`, `src/store/store.c` (open/close, lock, dir scan,
  create, header check, boot rule), `tests/test_store_header.c`.
- Makefile: add `STORE_LIB := src/store/store.c src/store/crc32.c src/capture/log.c`,
  a `$(BUILD)/test_store_header` rule (`-Isrc/store -Isrc/capture`), a
  `test-store-header` target, and `test: test-store-header`.
- Tests (all in a `mkdtemp` dir):
  - H1. Empty dir: creates `<s>.000001.mwlog` (32 bytes) and `<s>.lock`. Header
    fields and CRC are correct; boot_id matches /proc; counter == 1; `created == 1`.
  - H2. No `.tmp` is left behind. A stale `.tmp` placed beforehand is unlinked.
  - H3. Reopen in the same boot: same segment, header byte-identical, `created == 0`.
  - H4. Latest segment has another boot_id (crafted): it stays byte-identical, a new
    segment N+1 is created, `new_boot == 1`.
  - H5. Counter is 1 + max over live **and** `.corrupt-*`. With live 1 and quarantined
    4, the next segment is 5.
  - H6. Non-matching names (`bme280.abc.mwlog`, `foo`, the mlx files) are ignored.
    Counter 10 sorts after 9.
  - H7. EVERSION and ESENSOR: refused, every file in the dir is byte-identical,
    nothing is created.
  - H8. The second open while the first is held gives EBUSY. After close, it succeeds.
  - H9. fd count back to baseline after close; internal fds have FD_CLOEXEC; 1000
    open/close cycles cause no growth.
  - H10. Nonexistent dir, or a dir that isn't writable: error, nothing created.
  - H11. `store_header_check` unit cases: bad magic, bad CRC, record_size ≠ 32,
    counter ≠ filename counter.
- Commit: `M3: store segment create, header and per-boot selection`.

### Step 3: append
- Files: `store_append` in store.c, `tests/test_store_append.c`.
- Makefile: add the test_store_append rule and `test-store-append`. Add
  `memcheck: memcheck-store` plus a `memcheck-store` recipe running valgrind on all
  store test binaries built so far.
- Tests:
  - A1. Append 100, close, reopen: `records == 100`; bytes equal the input except
    crc32, which is valid.
  - A2. The caller's record is not modified.
  - A3. Bad magic, wrong sensor, or reserved ≠ 0: EINVAL, size unchanged.
  - A4. SYNTHETIC and READ_ERR flags and a = b = 0 are preserved bit-exact.
  - A5. Seq reset (1..5, 1..3), t_ns = 0, and t_ns going backwards are stored in
    arrival order.
  - A6. `RLIMIT_FSIZE` at header + 2.5 records, SIGXFSZ ignored: the 3rd append
    fails, size is 96, and the next reopen is clean.
  - A7. **Rotation across the boundary.** Test helper builds a same-boot segment 1
    with 8639 valid records (plain write, no fsync, so it's fast).
    1. Open, then append 2 records.
    2. Segment 1 now holds exactly 8640 (size 32+8640·32). Segment 2 exists, holds
       1 record, has the same boot_id, counter == 2 in its header.
    3. `store_counter` returns 2.
  - A8. **Full at startup.** A same-boot segment 1 with exactly 8640 records is
    left byte-identical. The first append lands in segment 2.
  - A9. **Rotation skips quarantined counters.** Live segment 3 is full and
    `bme280.000004.mwlog.corrupt-1` exists. Rotation creates 5.
    (Defensive: the "live is max" rule means this can only come from a crafted dir.)
  - A10. **Rotation failure.** Make the dir non-writable just before the 8641st
    append. It returns EIO, segment 1 is unchanged, and no `.tmp` is left behind.
  - A11. Reading every record back (raw file walk in the test) over segments 1–2
    gives every record in order, with none lost or duplicated at the boundary.
- Commit: `M3: store append with per-record fdatasync, rollback and rotation`.

### Step 4: recovery and quarantine
- Files: scan/truncate/quarantine in store.c, `tests/test_store_recovery.c`.
- Tests:
  - R1. For k = 1..31: 10 records plus k junk bytes → `records == 10`,
    `truncated_bytes == k`, size aligned.
  - R2. Last full record has a bad CRC → n-1 records, 32 bytes truncated.
  - R3. Last full record all zeros → truncated.
  - R4. Bad last record plus a partial tail → both removed.
  - R5. Middle record corrupt → renamed to `.corrupt-1`, byte-identical to before.
    New segment N+1 created; `quarantined == 1`; `bad_offset` correct.
  - R6. First record corrupt → quarantined as in R5.
  - R7. Last two full records bad → quarantined (one in-flight rule).
  - R8. Bad header CRC, or a file shorter than 32 bytes → quarantined.
  - R9. Quarantining the same counter twice (crafted pre-existing `.corrupt-1`) →
    `.corrupt-2`; the existing file is untouched.
  - R10. Torn tail on an **old-boot** latest segment is truncated, then a new
    segment is created.
  - R11. Recovery is idempotent: a second open reports `truncated_bytes == 0`,
    `quarantined == 0`.
  - R12. After recovery or quarantine, append works and the reopen is clean.
  - R13. `last_seq` / `last_t_ns` come from the last valid record.
- Commit: `M3: store tail recovery and corrupt-segment quarantine`.

### Step 5: reader
- Files: reader in store.c, `tests/test_store_reader.c`.
- Tests:
  - D1. Three segments (1, 2, 10): records come back in counter order, with the
    right counter and boot_id for each.
  - D2. A partial or bad last record ends that segment; the reader continues to
    the next; files unchanged.
  - D3. A sealed segment with a mid-file bad record: records before it are
    returned, the rest of that segment is skipped, `corrupt_tail_stops == 1`, the
    next segment is read.
  - D4. A bad-header segment is skipped (`bad_header == 1`).
  - D5. Segments 1, 2, 3, where 2 is `bme280.000002.mwlog.corrupt-1` with 10 valid
    records, a bad 11th, then 5 valid records after it:
    - The reader returns 1's records, then 2's first 10 with `src.quarantined == 1`,
      then 3's.
    - `salvaged_records == 10`, `quarantined_files == 1`.
    - The 5 records after the bad one are **not** returned.
    - The quarantined file is byte-identical afterwards.
  - D5a. Quarantined file with a partial tail only (no bad full record): all
    complete records are salvaged.
  - D5b. Quarantined file with a bad header, or shorter than 32 bytes: 0 salvaged,
    `bad_header == 1`, the walk continues.
  - D5c. Order within one counter: live file, then `.corrupt-1`, then `.corrupt-2`
    (crafted).
  - D5d. Writer-made quarantine end to end: corrupt the middle of the live segment,
    then `store_open` quarantines it (step 4 path). The reader salvages exactly the
    records before the bad offset (`bad_offset` from open_info matches).
  - D6. Empty dir: 0 records, no error.
  - D7. A reader open while the writer keeps appending sees only whole, valid
    records. The reader holds no lock: open the writer during a read, and it does
    not get EBUSY.
  - D8. No fd leaks after close.
- Commit: `M3: store multi-segment reader`.

### Step 6: daemon
- Files: `src/store/stored.c`, `tests/test_stored.c` (fork/exec of
  `build/capture --fake --binary` piped into `build/store`).
- Makefile: add `all: $(BUILD)/store` (prerequisite-only), the `$(BUILD)/store`
  rule, test_stored, and the valgrind fake pipeline line in a new
  `memcheck-stored` target.
- Tests:
  - S1. 50 fake records → 50 valid records, all SYNTHETIC; exit 0 on EOF.
  - S2. `--fake-drop 5:3` → READ_ERR records at seq 5..7.
  - S3. Capture without `--binary` → exit 6, segment is header only.
  - S4. Sensor mismatch → exit 6.
  - S5. 40 bytes then EOF → 1 record stored, partial dropped, exit 0.
  - S6. SIGTERM mid-stream → exit 0, clean reopen.
  - S7. Idle pipe with `--heartbeat-s 1` → heartbeats on stderr at start and within 2 s.
  - S8. SIGKILL mid-run, then restart → same segment (same boot), clean.
  - S9. Corrupt-middle latest segment → daemon quarantines, logs it, stores new
    records in N+1, exits 0 on EOF.
  - S10. Two daemons for one sensor → the second exits 5.
- Commit: `M3: storage daemon with heartbeat and supervisable exit codes`.

### After step 6 (not a unit test)
- On-Pi power-cut run: pull the plug N times. Each boot gets a new segment. Each
  restart loses at most 1 record per sensor and causes zero unintended quarantines.
- SIGKILL (S8) does not prove durability, because the page cache survives it.

---

## 5. Issues still flagged
1. **Salvage stops at the first bad record.** Valid records *after* a corrupt
   spot in a quarantined file are kept on disk but never reach analysis. Each
   record carries its own CRC, so skipping past the bad one is technically
   possible. That would be resyncing, though, which you ruled out. Likewise, a
   quarantined file with a bad header salvages 0, even if its records look fine.
1a. **Analysis must treat salvaged records as provenance-marked.** They come with
   `src.quarantined == 1`. Wet-hours and drying-time code should decide whether
   they count.
1b. **8640 equals 24 h only at the 10 s default.** It is a record count, not a
   time, so READ_ERR records count toward it and other intervals change the span.
   Segments don't align to midnight. If daily wet-hours wants day-aligned files,
   that would be a different rule.
1c. **"One file per boot" is now "a segment never spans a boot".** One boot can
   have many segments. Expect roughly 2 sensors × (1/day + reboots), about 250
   files per semester, and startup scans the directory. That's fine at that scale.
1d. **Startup scan cost** is now bounded to one segment (≤ 8640 × 32 B ≈ 276 KB).
2. **Sealed segments are never rescanned by the writer.** Bit rot in an old segment
   only surfaces when the reader stops there (D3). There is no alert path. Option:
   storage verifies old segments in the background and logs the result, without
   renaming anything.
3. **Unreadable boot_id (zeros) means every storage restart starts a new segment.**
   A crash loop then multiplies segments. That's harmless for correctness but
   produces noisy files. Linux always has the boot_id file, so this is low risk.
4. **A storage restart in the same boot appends to the same segment.** Segments
   are per boot, not per storage process. That is intended.
5. **The counter in the header must match the filename.** A hand-renamed segment
   gets quarantined, not silently trusted. Bytes are kept, so this is reversible.
6. **The one-in-flight tail rule (R7) depends on fsync-per-record.** Batching
   later means changing that rule.
7. **SD wear and fsync latency** (~0.2 GB/day at 0.1 Hz per sensor) is fine. But
   capture allows `--interval-ms 1`, which fsync can't keep up with. The max fsync
   ms in the heartbeat makes that visible.
8. **Pipe coupling:** storage EOF and capture EPIPE make them a pair. The
   supervisor (shared code) must restart them together, or hold the write end open.
9. **Reusing `src/capture/log.c`** compiles Ricky's file without modifying it.
   **The Makefile isn't in CLAUDE.md's ownership list.** Additive-only edits still
   change what `make test`/`asan`/`memcheck` run for Ricky, so tell him.
10. **Host byte order**, as in record.h. The header magic catches a cross-endian
    file as a bad header, which quarantines it with bytes kept.
11. **`.corrupt-<n>` interpretation:** I read `<n>` as a per-segment sequence
    number (1, 2, ...) appended to the segment name, e.g.
    `bme280.000002.mwlog.corrupt-1`. If you meant `n` to be the segment counter,
    the name becomes `bme280.corrupt-000002` and H5/R9 change accordingly.

## Verification
- Each step's tests are shown failing first, then `make && make test && make asan`
  (and `make memcheck` from step 3) are shown green on Linux before its `M3:` commit.
- Final check: the fake capture-to-store pipeline under valgrind, then the on-Pi
  power-cut run.
