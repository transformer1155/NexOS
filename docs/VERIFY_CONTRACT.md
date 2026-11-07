# Verification contract: exit codes and criteria hygiene

Status: agreed 2026-11-02 (Lead + glm-53), derived from the SFS name-truncation work
(`cooperation.run` permanently returned `code:2 template not found on SFS`).

This document exists because a contract that lives only in a script header is visible only to
the people who open that script.  The rules below should be quotable from one place.

## 1. Exit codes (layered on purpose)

| code | meaning | who uses it |
|---|---|---|
| 0 | every judged criterion PASSED | any checker |
| 1 | at least one criterion FAILED - takes precedence over 2 | any checker |
| 2 | no FAIL, but >=1 criterion is **INCONCLUSIVE** (judge-limited: this run cannot see it) | `take8_criteria.sh` |
| 3 | **VOID**: a meta gate refused to judge anything (premises not met) | `take8_criteria.sh` |
| 3 | the **tool** cannot verify (cannot read its inputs) | `tools/verify/preflight_image_sync.sh` |

The layer order **FAIL > INCONCLUSIVE > PASS** matters more than a shared number.  A checker
that reports PASS while hiding an INCONCLUSIVE has the same defect as one that reports PASS on
an empty sample.

### 1.1 The number 3 has two meanings - keep both, document both

* `preflight_image_sync.sh`: 3 = **the tool cannot verify** (its inputs are missing/unreadable).
* `take8_criteria.sh`: 3 = **the criteria were refused** (no private copy, image not in sync,
  diagnostic not compiled in).

Most teams either ignore this collision (and let callers guess) or force a single meaning
(destroying the layer order).  We keep the difference **and write it down**.  Conflict is not
the danger; silence about the conflict is.

## 2. Criteria hygiene - thirteen rules, each with the case that produced it
(a further subsection, 2.3, covers the collection side: whether a zero was collectable at all)

### 2.1 Per-criterion (R1-R9, R11-R13) - "is this single check trustworthy?"

### R1. An absence-criterion needs a positive control
`"[SFS-FIND] not found: ai/cooperation/..." is absent` proves nothing if the diagnostic was never
compiled in.  A control that reads the **serial** is itself void in that case, so anchor the
control one level earlier: `grep -F -c -- "[SFS-FIND]" build/kernel64.bin` must be >= 1.
*"Prove the mechanism is present before proving the phenomenon is absent."*

### R2. Scope the criterion to the semantic region, not the whole corpus
`[SFS-FIND]` also fires for unrelated files (`apps.overlay.json`, `vmlinuz`, `initrd.img`), so a
global count both false-passes and false-fails.  Extract the region with markers, never line
numbers: `awk '/START/{f=1} f{print} /END/{f=0}'`.  *"A global corpus cannot support a local
conclusion."*

### R3. An assertion belongs to the stage where its object is final
Comparing a pre-restore artefact against a post-restore baseline is a stage mismatch and fails
every time.  Split the assertion: source identity in the phase that changes the source, artefact
baseline in the phase that rebuilds the artefact.

### R4. Prove the evidence source can contain the evidence
A packer warning lives in the `sfs_gen.py` pack log, not in the kernel build log; a checker fed
the wrong log reports INCONCLUSIVE forever.  Report it as *"this log has no SFS: packing line -
you probably passed the kernel build log"*: a failure that names the likely mistake is worth more
than one that only says "unknown".

### R5. INCONCLUSIVE is never reported as PASS, and VOID stops the judgement
A meta gate that cannot judge must say so and stop, instead of producing six verdicts about the
wrong object.  (`RESULT: VOID (image not in sync) - every criterion below would describe the
WRONG kernel. Not judging any of them.`)

### R6. Assert the exit code AND the reason text
A test that only checks the exit code cannot distinguish "correct rejection" from "the script did
not understand its arguments" - that false pass happened to this tooling twice in one day
(`! cmd | tee` negating `tee` instead of `cmd`; a parser/usage mismatch making every invocation
hit the usage branch).  Assert both, and prefer separate weak judgements over one compound one:
when one breaks, the other still catches it.

### R7. Zero matches, zero bytes and "command failed" are three different states
`"I did not find it"` and `"it does not exist"` are not the same claim.  Before concluding,
separate three cases:

1. **the tool or path failed** - read the exit code and stderr (`No such file or directory` is a
   statement about the path, not about the object you were looking for);
2. **the pattern matched nothing** - a pattern can be wrong in a way that yields zero instead of
   phantom hits (wrong prefix, wrong file, wrong quoting); re-express it before believing zero;
3. **it genuinely does not exist** - only this case supports an "absent" conclusion.

Judge by **exit code and stderr**, never by "the output was empty".

Cases that produced this rule (all in one day):

* `grep -rn ... RULES.md` missing its `docs/` prefix -> `No such file` -> read as "that reference
  does not exist" (Lead).
* `\\|` used to find pipes matched **logical or** -> 1,757 phantom hits (driver-network).
* `bs=1` matched `bs=512` -> a search over the wrong term (driver-network).
* `0 bytes` matched `1800 bytes` -> a search over the wrong substring (driver-network).
* `[COOP]` treated as a character class -> 1,317 phantom hits (Lead).

Common thread: **"I did not find it" and "it does not exist" were treated as one thing.**

The **other direction** matters just as much: a non-zero count is not evidence either.  A hit can
land in a comment, in a help string, or inside a longer token - and a checker that audits "do not
do this" rules will always find at least one comment saying "do not do this".

So for every hit, ask *"is what I matched the thing I was looking for?"* and print the raw line:

```bash
# hits in code only: drop comment lines (shell/C/C++/block-comment styles)
grep -nE 'PATTERN' file | grep -vE '^[0-9]+:[[:space:]]*(#|//|\*|--)'
# and always print the matched line itself, so a human can see code vs. prose
```

This is the standard form for any "banned pattern" audit: exclude prose, and show the samples.

**And prove that the scope you searched covers the claim you are making.**  Searching a
subdirectory is not searching the repository; searching one file is not searching both
implementations.  Zero hits in a region you never entered say nothing about the whole.

Cases (all three in one day, all of the same shape):

* scanning only `sfs_files/` and reporting "14 long paths" (the real answer was 15 - `linux_root/`
  held three more);
* grepping only `.attic64/kernel64.cpp` and concluding "the repository has no heap window" - the
  window and its RAM-backing check live in `kernel.cpp` (the 32-bit kernel);
* grepping only the `ai/cooperation/` prefix and asserting a path-length limit.

This is R7 strengthened: not only "look at stderr", but "show that your search covers your
conclusion".

**Collection side (2.3).**  R7 governs how to *interpret* a zero; 2.3 governs whether the zero was
*collectable at all*.  A zero produced by a late-armed breakpoint or a too-short ring is R7 case (1)
in disguise - the tool never ran over the object - and it is the most common way this rule gets
violated on live instrumentation.

### R8. All-caps abbreviations must be matched case-sensitively, with word boundaries
Abbreviations used as tokens (`FAULT`, `CR2`, `VMEXIT`, ...) hide inside ordinary English words,
so a case-insensitive search mixes them with prose:

```bash
grep -iE 'FAULT|PANIC' log      # matches de-fault, theme_default, ...  -> phantom hits
grep -nE '\bFAULT\b|\bPANIC\b' log   # 0 hits here: the right form
```

The same family as the `[COOP]` character class and `bs=1` vs `bs=512`: the pattern language is
not the literal semantics.  Here the confusion comes from the abbreviation being *embedded in a
word*.  Note that `grep -F` with the exact token is also safe (it is literal and case-sensitive).

### R9. Never say "never" without naming the condition that would break it
`"the prompt never reaches the serial"` was half true and therefore dangerous: it is true only
while nobody sends a newline.  Injecting a single carriage return produced `login:` three times
and three `Login incorrect` replies - the kernel was responding all along.

An absolute claim must be accompanied by the condition that would falsify it.  If you cannot name
that condition, the honest form is *"not observed under these conditions"*, not *"never happens"*.
An over-broad mechanism claim is worse than a wrong one, because it is right most of the time and
so never gets suspected when its counterexample appears.

Prefer an **existence proof** ("inject input, observe the response") over a **non-existence
inference** ("I did not see X"): the two differ by an order of magnitude in reliability.

**Numeric spaces (the same number, different meanings).**  A value can be an opcode, a port, a
register offset or an address - and a bare search cannot tell them apart:

```c
/* --- jcc --- */
case 0x70: case 0x71: ...        /* opcode 0x70 inside switch(op)  -- NOT port 0x70 */
```

A search for `0x70` in `webemu/src/cpu_min.c` returns that opcode label and looks like "webemu
handles the CMOS ports 0x70/0x71"; the real port dispatch (`out8` / `in8`) contains no 0x70 at
all.  State which space a number belongs to and read it in the context of that space (the
enclosing construct: `switch(op)` vs `out8(port, ...)`).

### R11. The width of the criterion must equal the width of the proposition
A criterion that is too narrow misses cases (a false pass); one that is too wide invents
violations (a false failure).  Both were measured on the same 1930-function kernel, for the same
proposition ("does every function that uses EBP honour the prologue contract?"):

| criterion | functions matched | violations | verdict |
|---|---|---|---|
| window = first 5 instructions | 1220 | 9 | **false** - GCC emits early-out fast paths *before* the prologue (`fb_console_init` pushes EBP at instruction 12) |
| pattern = `(%ebp)` (EBP-relative memory operand) | 1220 | 0 | correct width for the proposition |
| pattern = `%ebp` anywhere in an instruction | 1501 | 3 | **false** - the three `naked` functions in `linux_compat.cpp:238/254/277` own their register contract on purpose |

So: the criterion must cover exactly the class of objects the proposition is about - no narrower,
no wider.  When widening a pattern, say in writing which objects are deliberately out of scope,
because the next reader may otherwise "fix" them (adding a prologue to `mini_longjmp` destroys
setjmp/longjmp).

### R12. Never infer a register value's meaning from its shape
A value that *looks* like a base address or a pointer may be a coordinate or a colour.  Before
interpreting any register in a snapshot, establish the parameter/local mapping **of that function**:

```c
void mh_draw_line(int x0,int y0,int x1,int y1,uint32_t c);
/*  edx <- 0x8(%ebp)  = x0     ecx <- 0xc = y0
 *  edi <- 0x10(%ebp) = x1     ebx <- 0x14 = y1     esi <- 0x18 = c (colour) */
```

A snapshot read as `edx=0x10c10, edi=0x10000, esi=0x1090c, eax=0` was therefore **x0 = 68,624,
x1 = 65,536, colour = 0x1090c`** - garbage coordinates from a corrupt frame, NOT "memcpy dst base
0x10000, buffer pointer 0x1090c, destination 0x10c10".  Two causal chains were built on the wrong
reading before the parameter map (plus a symbol-table check that 0x1090c lies *between* two
functions and is no variable) refuted it.

This is R8's numeric-space branch one level up: there the same number was an opcode vs a port, here
the same register means a different argument in every function.  The shape of a value is not its
semantics.

*Meta-note, recorded because it is uncomfortable:* this rule was needed by one of this document's
own authors, after writing the other eleven.  A written contract does not stop its author from
violating it; it only lets a second reader catch the violation.  The corrective value of a contract
is in the eyes it equips, not in the hand that wrote it.

### R13. Before concluding, name the object and the basis
**This one is a precondition, not a sibling:** R1-R12 all assume that the object has already been
identified and that its numbers sit in the right coordinate space.  Every hour actually wasted on
this project was spent after that assumption was skipped.

Before any "X is wrong" conclusion, answer three questions:

1. **Object** - which object did I read (file? sector? register? memory region?), and what is its
   identity?  Evidence: a sha, a symbol address, an LBA number, a register name.
   Every reading also carries **the time it was taken**; a receiver outside that window must
   re-measure rather than reuse the number (a stale number and a remembered number fail the same way).
2. **Basis** - which coordinate space is this value in?  The confusable pairs seen here:
   * canonical LBA vs physical LBA (the delta),
   * file offset vs image offset (the load base),
   * stack address vs global address.
3. **Counterexample** - is there existing evidence that would falsify it?  If you cannot say
   "no counterexample", you may not conclude.

*Case A (one agent, ten consecutive misjudgements):* x87; `halt`; the exception dumper; sector reads;
base 0; directory parsing; a missing `data_lba` delta; an LBA-33 offset; too coarse sampling; a 4-byte
storage limit.  The agent's own summary: **every one of them was "concluding before confirming the
object and the basis"**.

*Case B (the other author, three retractions in one hour):* byte order assumed to have one of two
forms - a new any-width tool showed **neither** form was present (the data never reached RAM at all);
`size == 0` asserted while ignoring that ATA had read 8,818 sectors; a "MKFS priority" hit asserted
while the serial plainly said `[MKFS] Not formatted` (so that branch could not fire).
*Case C (the same author, once, in a status message):* a progress sync reported "**11** R headings"
- a value carried from memory - while the file on disk already had **13**.  The judgment came from
memory, and memory is not the object.  This case is listed separately because it is the most
*ordinary* one: status messages, verbal hand-offs and progress updates happen constantly, and they are
exactly where a remembered value slips in as if it were a measurement.


Why this is distinct from R12:  R12 governs **what a value means** once you have the object;
R13 governs **whether you have the object at all**.  R13 comes first.

### 2.2 Per-experiment (R10) - "can this set of runs be compared at all?"

Every cell of a matrix can be computed correctly while the matrix as a whole is not a valid
comparison - "each cell is right" and "the table is right" are two different claims.

### R10. The decisive experiment changes one variable - and the matrix must be complete
Three logs were compared as if they formed a matrix: `-m 256` (no input), `-m 512` (no input),
`-m 512` (input).  The missing cell was **`-m 256` with input**, and without it the conclusion
"a 0 KiB heap stops the guest" was unsupported: `-m 256` and `-m 512` behaved *identically*
when no input was sent, so the heap size was not shown to matter at all.

Before comparing runs, lay them out as a matrix and name the missing cells; the smallest cell that
separates the competing explanations is the next experiment - not the most convenient one.  A
column that requires human judgement (did the guest respond?) should be reduced to a machine-checkable
cell (`responded=yes(3)`), so the whole matrix can be read off in one line.

*Outcome of this case (recorded so the method is not over-claimed).*  The missing cell was not, in
the end, the deciding evidence: reading the two logs line by line showed that `-m 256` (heap 0 KiB +
2 OOMs) and `-m 512` (heap 128 MiB) **both** reached the `=== NexOS Security Login ===` banner.  So
the heap size did not govern "reaching login" at all, and the matrix cell was unnecessary.  R10 still
stands - naming the missing cell is how one *notices* that a comparison is incomplete - but the lesson
here is that the ordering of lines inside a log can be stronger evidence than a missing experiment.

### 2.3 Collection side (instrumentation) - "did my probe see the whole window?"

R7 governs how to interpret a zero.  This subsection governs something earlier: **whether the zero
was collectable at all.**  Live instrumentation fails in this direction far more often than it fails
by misreading a value, and the resulting "I never saw it" is then read as "it does not exist".

Three measured rules, each replacing a habit that actually broke:

**C1. Use a ring log that starts from the beginning, instead of arming after you spot a serial line.**
The 32-bit `serial_putc` only flushes on `\n` (or a 256-byte overflow), so the serial you read
already lags the guest.  Arming a breakpoint when `[VFS] mounted` appears arms it **after** the code
you wanted - the breakpoint never fires, and the empty capture looks like "the event never happened".
*(Two occurrences in one night.)*

**C2. Record emulator-internal invariants (si/di/es/ds/counters/registers), instead of reading
function arguments off the stack.**  Under an `.isra`-cloned function the argument-passing
convention is not the one in the source signature: a stack-order read produced `n = 0x15E360`,
a plausible-looking but meaningless value.  Registers the emulator maintains are invariant under
cloning; stack layout is not.

**C3. Use a dedicated counter per target, instead of keeping the last N entries of a ring.**
A 32-entry ring was not enough; a 512-entry ring could still drop items.  One counter per target
(here: per LBA) cannot drop anything, and its answer is a number you can compare rather than a
sample you must hope is representative.

### Common failure mode (one sentence)

**All three are collection failures caused by window or timing, and a collection failure is then
read as non-existence.**  The probe was late, too short, or asked the wrong frame - and the empty
result was reported as an absence of the event.

So before reporting that something never happened, state **which window your probe covered** and
**why that window contains the event if it happened**.  An uninstrumented interval is not evidence;
it is silence.

## 3. Applying this to a new checker

1. List the premises; make each one a **meta gate** that VOIDs the run (exit 3) instead of a FAIL.
2. For every criterion, name the object it judges and the stage where that object is final.
3. For every absence-criterion, add a positive control anchored at compile time or in the artefact.
4. Scope counts to a semantic region; never count over a global corpus.
5. Prove the log/tool you read is the one that can contain the evidence.
6a. Count FAIL and INCONCLUSIVE separately; exit 1 / 2 / 0 in that order.
6b. Before trusting a zero result, separate "tool failed" / "pattern missed" / "absent" (R7).
6c. Before trusting a non-zero result, print the matched lines and drop comment-only hits (R7);
    a rule that forbids a pattern is a rule whose own comment contains that pattern.
6d. For all-caps tokens use case-sensitive + word-boundary matching (R8).
6e. State the conditions of every absolute claim; prefer an existence proof over a non-existence
    inference (R9).
6f. Before submitting a rule, have someone who did **not** write it independently verify one
    instance.  "I wrote the rule" is not evidence that "I follow the rule".
    *Case:* R12 was violated by one of the authors of R1-R11, after writing them; what caught it was
    another agent's register snapshot plus a parameter-map check.  This step is about the
    **process**, where R10 is about the **evidence**: R10 asks whether my output covers the claim,
    6f asks whether my checking was itself checked - by eyes that are not mine.
6g. Before trusting a live-instrumentation zero, state which window the probe covered and why that
    window contains the event if it happened (2.3, C1-C3).  An uninstrumented interval is silence,
    not evidence.
7. Dry-run the checker on a **known-failed** state: a checker that has never said FAIL proves nothing.

## 4. References

* `tools/verify/preflight_image_sync.sh` - image-vs-artefact byte comparison (exit 0/1/3).
* `_aa_out/take8_criteria.sh` - worked example: 7 judging items, 2 meta gates, 4 exit codes, dry-run
  validated against a known-failed state.
* `_aa_out/sfs_truncated_names_techdebt.md` - the register + its six-clause constraint system.
* `_aa_out/_sfs_longname_scan.sh` - the double-volume scan that feeds that register.
