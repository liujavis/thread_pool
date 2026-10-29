# Seminar Talk Prep: Can an AI Agent Do Performance Engineering?

Working plan for an internal seminar talk built around
[intel/intel-performance-skills](https://github.com/intel/intel-performance-skills).
This file is written as instructions for Claude Code (running on a company machine)
and as a checklist for me.

**Thesis of the talk:** Does an agent (Claude Code) diagnose and fix C++ performance
problems better when it has structured perf skills? Measure it, A/B, on our hardware.

---

## Ground rules (read first)

- This repo contains **only public / self-written material**. Nothing from the company
  codebase, results, or machine details is ever committed back here or pushed outside.
- All benchmarks, experiment results, and transcripts live in an **internal** location.
- Phase 4 (production code) only starts after I confirm approval. Read-only review;
  the agent does not modify production code.
- Pin the intel-performance-skills commit hash used, so results are reproducible.

---

## Phase 0 — Environment check

Goal: know what perf can actually do on this machine before building anything.

Run and record into `results/env.md`:

- [ ] CPU: `lscpu` (vendor, model, microarchitecture, SMT, NUMA nodes)
- [ ] Kernel: `uname -r`; perf: `perf --version`
- [ ] `cat /proc/sys/kernel/perf_event_paranoid` (need ≤ 1 for most of this, ideally -1 or 0)
- [ ] HW counters work: `perf stat -e cycles,instructions,cache-misses,branch-misses -- true`
- [ ] Topdown works: `perf stat --topdown -a -- sleep 1` (Intel) — note if unsupported
- [ ] c2c works: `perf c2c record -- sleep 1 && perf c2c report --stdio | head` (needs PEBS on Intel / IBS on AMD)
- [ ] Are we in a container/VM? (PMU access is often missing there)
- [ ] Compilers: `g++ --version`, `clang++ --version`, cmake version
- [ ] Claude Code version and model used (record for the talk)

If topdown or c2c doesn't work, stop and tell me — it changes which demos are feasible.

---

## Phase 1 — Get the skills

- [ ] Clone `intel/intel-performance-skills` into `third_party/`, record commit hash in `results/env.md`
- [ ] Read all three `SKILL.md` files (`linux-perf`, `performance-patterns`, `phoronix-test-suite`)
      and write a 1-page summary in `notes/skills-summary.md`: what flows exist, what the
      pattern catalog covers, what the detection signals are, any x86/Intel-specific assumptions
- [ ] Do **not** install the skills into `~/.claude/skills/` (user-level) — they must be
      project-level only, so the A/B experiment can turn them on and off cleanly

---

## Phase 2 — Build the benchmarks

Layout:

```
bench/
  CMakeLists.txt
  common/harness.h          # timing, thread launch, core pinning, repetitions
  b1_false_sharing/  bad.cpp  fixed.cpp
  b2_shared_counter/ bad.cpp  fixed.cpp
  b3_spinlock/       bad.cpp  fixed.cpp
  b4_serial_accum/   bad.cpp  fixed.cpp
  b5_branch_mispred/ bad.cpp  fixed.cpp
```

Build flags: `-O2 -g -fno-omit-frame-pointer -march=native` (no `-ffast-math`).
Each binary takes `--threads N --iters M`, prints wall time and ns/op, exits 0.
Harness pins threads to distinct physical cores (avoid SMT siblings); runs 5 reps, reports median.

Make the code look like plausible trading-system code (stats, counters, order/price data),
not textbook toy code — but self-written, nothing copied from company code.

| ID | Scenario (trading flavor) | Bad version | Fixed version | Expected perf signature |
|----|---------------------------|-------------|---------------|-------------------------|
| B1 | Per-thread stats (msgs processed, bytes) in a contiguous struct array | Structs packed, several threads' counters share a cache line | `alignas(64)` / padding per thread | `perf c2c` high HITM on one line; throughput drops as threads increase |
| B2 | Global "orders processed" counter on the hot path | Every thread `fetch_add` on one `std::atomic` | Thread-local count, flush every K ops | `lock add` dominates annotate; true sharing in c2c |
| B3 | Tiny critical section (update best bid/ask) protected by a spinlock | Test-and-set spin (`exchange` in a loop) | Test-and-test-and-set + `_mm_pause` | `lock xchg/cmpxchg` cluster; scaling collapses with threads |
| B4 | Rolling sum / dot product over float price×qty arrays | Single scalar accumulator (no -ffast-math, so no reassociation) | 4–8 independent accumulators and/or explicit SIMD | Low IPC with low cache-miss rate; `vaddss`/`vfmadd` chain in annotate |
| B5 | Filter orders by an unpredictable condition (e.g. side/price threshold on random data) | Data-dependent branch in the hot loop | Branchless (arithmetic/mask) version | High `branch-misses`; Bad Speculation in topdown |

B5 is deliberately **outside** the skill's pattern catalog — it tests how the agent behaves
when the skill has no ready-made answer.

For B5, check the disassembly: if the compiler already turned the branch into `cmov`,
make the condition more complex until the bad version really branches.

Validation (must pass before Phase 3):
- [ ] Each bad version shows the expected signature (save the `perf stat` / `c2c` / `annotate` output)
- [ ] Each fixed version is measurably faster; record baseline speedup in `results/baseline.csv`
- [ ] Thread counts for B1–B3: run at 1, 2, 4, 8 (and more if the machine has cores)

---

## Phase 3 — The A/B experiment

Two configurations:
- **A (no skills):** workspace with no skills anywhere (check `~/.claude/skills/` and `.claude/skills/` are empty of them)
- **B (skills):** same workspace + the three skills copied into `.claude/skills/`

Rules:
- Each run uses a **fresh copy** of only the `bad.cpp` + CMake (the `fixed.cpp` must NOT be
  visible to the agent — separate directory, not in git history)
- Fresh Claude Code session per run; same model for all runs
- 3 runs per benchmark per configuration → 5 × 2 × 3 = 30 runs
- Same prompt for A and B. Two prompt levels (use Level 1 for all; Level 2 optional):
  - **Level 1 (vague):** "This program is slower than it should be. Find out why and make it faster. Report before/after numbers."
  - **Level 2 (hinted):** "This program gets slower when I add threads. Find out why…" (B1–B3 only)
- Save every transcript to `results/transcripts/<bench>_<config>_<run>.md`

Record per run in `results/ab_results.csv`:

```
bench,config,run,used_perf(y/n),perf_commands,root_cause_correct(y/n/partial),fix_correct(y/n),speedup,turns,wall_minutes,notes
```

Things to note qualitatively (these make the best slides):
- Did it measure first, or jump straight to editing code based on a guess?
- Did it verify the fix with a measurement?
- Wrong turns, hallucinated perf events, misread output
- B5: what does config B do when the catalog doesn't have the pattern?

---

## Phase 4 — Production code review (ONLY after approval)

- [ ] Confirm with compliance/manager that running Claude on production code for this purpose is OK
- [ ] Pick 1–2 hot-path components I know well (so I can judge findings)
- [ ] Agent does a **read-only** review using the `performance-patterns` skill: list suspected
      issues with file/line, pattern name, and the detection signal it would expect in perf
- [ ] I verify each finding with an actual perf measurement; classify as: real / already known / false positive
- [ ] Report for the talk only as aggregate counts and anonymized/generic examples — no real code on slides unless approved

---

## Phase 5 — Draft our own team perf skill

Using the intel repo as a template, draft `skills/team-perf/SKILL.md` (internal only) covering
the knowledge that is specific to us, e.g.:
- How to build with symbols / profiling flags in our build system
- Which machines/cores to use for benchmarking; core isolation and pinning conventions
- How our internal benchmarks / replay tools are run
- Known pitfalls (hugepages, NUMA placement, logging on hot path, etc.)

I will fill in the actual content — the agent drafts the structure and asks me questions.

---

## Phase 6 — Slides (~45 min)

1. Motivation: why perf engineering is hard to scale (2 min)
2. perf / topdown methodology: IPC → topdown → annotate / c2c (10–12 min)
3. What a "skill" is; the intel repo; the pattern table (5 min)
4. Benchmarks + live `perf stat` demo on one bad/fixed pair (5 min)
5. A/B results: table + 2–3 transcript highlights incl. a failure (12–15 min)
6. Production code review: aggregate findings (if approved) (3–5 min)
7. Takeaway: write our own skill; what agents are / aren't good at here (3 min)

Agent sessions are shown as pre-recorded clips/screenshots — never live.

---

## Status checklist

- [ ] Phase 0 env check
- [ ] Phase 1 skills reviewed
- [ ] Phase 2 benchmarks built + validated
- [ ] Phase 3 A/B runs done (30)
- [ ] Phase 4 approval obtained / review done
- [ ] Phase 5 team skill drafted
- [ ] Phase 6 slides
