# PROMPTLOG -- Luke Sellers

Individual log. Written by me, about my own sessions. Due in full at M5
(6-10 annotated episodes), but written as it happens, not reconstructed.
By M5 this must include at least: one revised plan, one rejected diff,
one tool-output debugging loop with real hardware evidence, and one
review of my partner's work.

Template for each episode -- delete this block once the first real one
is written.

## Episode 1 -- <date> -- <one-line title>

**Context.** Review our code from M0/M1 again to make sure we were moving in the right direction for M2/M3.

**What I asked.** I asked claude to go over every file and relate it back to CLAUDE.md and PROBLEM.md as well as identify potential gaps.

**What came back.** There was a line-ending churn from the intital commit that changed every file into a CRLF. There was a typo in .gitignore. And there was an out of data README status. Claude came back with a plan to add * text=auto eol=lf to .gitattributes, run git add --renormalize ., remove the broken .gitignore line, make one commit, and not rewrite d0fb138. 

**My judgment.** I changed the identifier from M1 to M2. I also set my git name and email before doing anything else since it could have confused mine and rickys commits. I accepted not rewriting history.

**Evidence.** The dmesg, i2cdetect, timing capture, or test output that
settled it. Not a description of evidence.

**Outcome.** The commit landed in an .

## Episode 2 -- 10/27 -- Explore session for the storage layer

**Context.** Since I am in charge of src/store I wanted to do an explore session to know exactly what capture sends and what storage has to guarantee.

**What I asked.** In plan mode making no edits I asked claude to read CLAUDE.md, PROBLEM.md, record.h, makefile, and tests and capture.c/log.c. I asked it to report how capture emits records with file and line, what PROBLEM.md says about power loss citing direct quotes, what could be broken in the makefile from my changes, and questions it had about design choices.

**What came back.** A report with line citations and four groups of questions on the file layout, fsync, recovery, and one writer vs many. 

**My judgment.** I checked the citations against the repo and the quotes against PROBLEM.md to make sure they matched which they did. It also said that PROBLEM.md never said how much data we may lose so I had to decide that myself, and I chose to go with at most one record per sensor.

**Evidence.** How capture emits records:

- Binary output is opt-in. By default stdout gets readable text lines
  (capture.c:186). Raw 32-byte records come out only with --binary
  (capture.c:7-8, :112).
- Write path: each record is written as the raw struct in host byte order,
  with no header or framing (capture.c:182). stdout is flushed after every
  record (:199).
- seq counts from 1 and starts over every time capture restarts (:291).
- crc32 and reserved are left at 0. Storage fills in crc32 (record.h:35).
- SIGPIPE is ignored (:258). If storage dies, capture gets a write error and
  exits with code 3 (:329-333).

What storage must guarantee (from PROBLEM.md): 

"weeks of wetness history are the product itself and a bathroom outlet
losing power mid-write must not corrupt or erase them."

Not specified anywhere: how much recent data we're allowed to lose in a
power cut..

**Outcome.** Saved to docs/store-explore-notes.md.

## Episode 3 -- 10/27 -- Storage plan and revisions

**Context.** Exploration done, need storage plan before code written.

**What I asked.** Still in plan mode making no edits I asked claude to plan for my decisions. fsync each record, one append only log per sensor, recovery truncates a torn tail and doesnt resync, one process per sensor, CRC in src/store, record.h and src/capture remain the same. Also asked for a file format, API, recovery algorithm and tests written first, and any issues it foresaw with my decisions.

**What came back.** A full plan with 32 byte header, a store API, a recovery algorithm and 45 tests. It then listed 10 issues with my decisions to take into consideration.

**My judgment.** I did not approve right away. I sent it to plan again with four changes, one file sensor per boot with ECORRUPT renaming the file to track it and starting fresh segment, commit with M3 instead of M4, any edits to the makefile only add lines not delete any, and split commits into segments so testing can be done before each commit. The plan came back with necessary changes and an issue that I reviewed in a seperate claude session before changing. It said that make won’t allow a second recipe on an existing target, so it adds a new test target per step. On a Pi that stays powered for weeks, one boot is one huge segment, so one bad bit would still quarantine weeks of data. I added rotation (a segment holds at most 8640 records) and made the reader salvage a quarantined file up to its first bad record. I then chose not to let the reader skip past corruption, to keep the no-resync rule. I also declined auto-approve for the edits.

**Evidence.** Plan 1 (Claude Code):

F1. boot_id in a per-sensor header records only the boot that created the file.
A log lives for weeks across many reboots, but the header is written once. It
cannot tell you which boot a record came from ... If per-boot attribution
matters, the options are: (a) one file per sensor per boot:
bme280.<bootid>.mwlog (b) drop boot_id

F2. Refuse-on-corruption plus a single forever-file can stop a sensor
permanently. One flipped bit anywhere in weeks of SD-card data makes storage
exit 5 on every restart. Capture then dies with EPIPE (capture.c:258), so that
sensor is down until a human intervenes. For the BME280 that means no dew
point, which means the whole product is down. That conflicts with CLAUDE.md:
"degrade the product, never stop it". Mitigations that keep "no resync"
intact: ... or storage, on ECORRUPT, renames the log to *.corrupt-<time>
(preserving every byte) and starts a fresh file. The second is still "refuse
to resync", but it needs your OK.

Revision prompt (v1 to v2):
A few revisions, stay in plan mode no editing. 1. Please combine F1/F2/F3,    
one file per sensor per boot named sensor.counter.mwlog with bootid kept in   
the header. On ECORRUPT rename the file to *.corrupt-<n> keeping every byte   
and starting a fresh segment. Reader takes a dir+sensor and walks segments in 
counter order. Update API, recovery algorithm, and affected tests             
accordingly. 2. Use the prefix M3 not M4 for the commit since that is what we 
are working on. 3. Keep makefile edits minimal and only add to it do not      
delete anything. 4. Split the implementation so that we can run tests. Split  
into CRC, create/header, append, recovery, reader, daemon with each having    
its own tests and commit.

Plan 2 (Claude Code):
  3. Makefile: I only add lines and never edit or delete one. Make doesn't
  allow a second recipe for an existing target. So each step adds its own
  test-store-<step> target, plus a one-line test: test-store-<step> that
  hooks it into the existing test and asan.
  Still flagged: Quarantined data is hidden from analysis: the reader skips
  quarantined files, so analysis loses that boot's good records too.

Second revision prompt (v2 to v3):
Two more changes before approval stay in plan mode. 1. Add a rotation,        
segment holds at most 8640 records, and the next append goes to counter+1.    
Update the segment rule and add a test. 2. The reader reads a quarantined     
(*.corrupt-<n>) file up to its first bad record then continues to the next    
segment. Report how many records were salvaged and keep everything else. 

Plan 3 (Claude Code):
  Rotation: a record goes into the live segment only if that segment belongs
  to this boot and holds fewer than 8640 records. Otherwise the next segment
  is created, fully, before the record is written.
  Tests: A7 the 8640 -> 8641 boundary; A8 a segment already full at startup;
  A11 no record is lost or duplicated across the boundary.
  Reader: D5: a quarantined file with 10 good records, a bad 11th, then 5
  good: exactly 10 are salvaged, the 5 after the bad record are not returned,
  and the file is unchanged.
  New issue: Records after the bad spot stay unreadable: they're kept on disk
  but analysis never sees them. Reaching them would mean skipping past
  corruption, which you ruled out.

**Outcome.** Saved to docs/store-plan.md.

---
