# LPTA — LLVM Pass Transformation Analyzer

## Hackathon Plan & Execution Roadmap

**Goal:** Understand compilers for real by building the best possible version of this tool.
Deadlines dropped; the tool and your head are the deliverables.
**Constraints:** No deadline, no artifacts-for-their-own-sake. ~10 days at speed, phases not
dates. The deliverables are the tool and your head. HTML reports, READMEs-as-milestones, demo
videos, polish phases — all deleted. What remains is the problem statement, built well.

---

## 1. Vision — the problem, from first principles (read this; §3 and §6 are sealed)

LLVM optimizes through 150+ passes at `-O2`. Each pass reads IR, rewrites it, and hands it
to the next pass. The final binary is all of them composed. The pipeline is opaque: no
existing view answers *"which pass did what, and how much did it matter?"*

**Four pains — the actual issues this causes:**

1. **Attribution.** Your hot loop looks nothing like your source. Which passes reshaped it?
   Today: manual archaeology through IR dumps.
2. **Quantification.** Did that pass matter, or just rename things? Today: eyeballing.
3. **Composition.** 150 small edits compose into the final binary. No unified story exists.
4. **Regression.** A compiler upgrade slows you 5%. Which pass's behavior changed? Today:
   bisection (finds *crashes*, not changes) plus hand comparison across runs.

Who bleeds: compiler devs ("did my new pass do what I intended?"), performance engineers
("what transformed this loop?"), anyone hit by "the compiler made my code worse."

**Why existing tools don't suffice:**
- `-print-after-all`: a complete firehose — zero structure, zero quantification.
- `-time-passes`: measures time, not effect. Slow ≠ impactful; fast ≠ harmless.
- `-Rpass` remarks: scattered, opt-in, per-decision. They never compose into a pipeline story.
- And even raw before/after IR resists naive comparison (renaming, reordering) — so "just
  diff it" understates the problem. (This is pain, not prescription. Your Phase 0 design
  must face it honestly.)

---

## 2. Definition of Done (what "genuinely good" means)

- The analyzer works end-to-end: per-pass snapshots, honest impact scores, one-screen terminal view
- **You authored a real optimization pass** (`PassInfoMixin`, correct `PreservedAnalyses`), ran it
  through your own pipeline, and read its impact score off your own tool — the closed loop
- You can narrate what SROA, GVN, InstCombine, SimplifyCFG, and the Inliner actually do —
  invariants, costs, triggering IR patterns — from having read their `run()` functions
- Every line typed by you; reference code consulted only after attempting each piece

---

## 3. Architecture

> ⛔ SEALED until you pass Phase 0's exit test. This is *one* design, not *the* design.
> Reading it first will anchor every decision you make afterward. `src/` is sealed too:
> the binary is a black box you may run, never source you may read — until the phase
> that builds that piece.

```
input.c ──► clang (emit IR) ──► LPTA
                                 │
                                 ├─ TargetMachine + TargetLibraryInfo setup
                                 ├─ PassBuilder
                                 │    └─ register PassInstrumentationCallbacks
                                 │         ├─ beforePass: snapshot Module (IR string)
                                 │         └─ afterPass:  snapshot Module
                                 ├─ run pipeline (default<O2>) in memory
                                 │
                                 ├─ Diff Engine
                                 │    ├─ structural delta (fns/BBs/insts added/removed)
                                 │    ├─ semantic signature distance (opcode histograms)
                                 │    └─ cost delta (TargetTransformInfo)
                                 │
                                 ├─ Codegen step (M2b)
                                 │    └─ lower final IR through the backend (llc)
                                 │       → .text size, instruction count, "end-state"
                                 │
                                 └─ Report Generator → HTML (table + timeline + narrative + codegen impact)
```

**Component layout (suggested):**

```
lpta/
├── CMakeLists.txt
├── README.md
├── src/
│   ├── main.cpp          # CLI entry, pipeline setup
│   ├── instrumentor.cpp  # PassInstrumentation callbacks + snapshots
│   ├── diff.cpp          # structural + semantic metrics
│   ├── score.cpp         # composite significance scoring
│   └── report.cpp        # HTML generation
├── tests/                # example C kernels
└── out/                  # generated reports
```

---

## 4. Fuel (not curriculum — grab what each phase needs)

No stages, no schedule. Two resources, used on demand:

**A. Scoped source build (background task, kick off first).** Previously out of scope;
now in, because reading pass source with a debugger beats reading docs about passes:
```bash
git clone --depth 1 -b llvmorg-22.1.8 https://github.com/llvm/llvm-project.git ~/llvm-src
cmake -S ~/llvm-src/llvm -B ~/llvm-src/build -G Ninja \
  -DLLVM_TARGETS_TO_BUILD=X86 -DCMAKE_BUILD_TYPE=Release
ninja -C ~/llvm-src/build opt
```
One target only (X86) keeps it to ~1-2h on a decent machine. Let it compile while you do
Phases 1-5 against system LLVM. From Phase 6 on, read `llvm/lib/Transforms/*` from this
tree and step passes in `gdb`/`lldb` when curious. If the machine chokes, abandon without
guilt — system LLVM plus online source is enough.

**B. Permanent shelf (depth when hungry, never assigned):**
Cornell CS 6120 · Cooper & Torczon *Engineering a Compiler* · Appel *Modern Compiler
Implementation* · Lattner's talks + the 2002 MSR paper · Dragon Book optimization chapters.

---

## 5. The Path (phases, not days — ~10 days at speed)

Move on when you pass the exit test. Roughly a phase a day; Phase 6 and 10 deserve more.
Rules hold throughout: you type every line; reference code only after attempting; the
verify command *is* the learning — never skip it.

### Background task — source build (not a phase; runs in parallel)
Kick off §4A immediately, then forget it until Phase 6. Costs nothing while it compiles.

### Phase 0 — Derive (before everything else, ~2-3h, pen and paper)

You know the pains (§1). Now derive the solution yourself — using only tools you already
know (`opt`, `clang`, shell, text):

1. For each of the four pains, write down: *what would a tool need to capture or compute
   to answer this?* (One sentence per pain. No implementation talk.)
2. Sketch a design: how would you observe every pass's effect? How would you decide whether
   a change was "big"? How would you show 150 passes' effects as one story? Where does your
   sketch break? (Write the breakage down — honest failure modes are the point.)
3. Bring the sketch to me. We compare notes — and only then do §3 and §6 unseal.

*Exit:* a one-page design in your own words, produced before seeing mine. No code, no APIs.

### Phase 1 — See
You read IR. Now watch it change: `-O0` vs `-O2` emit, `opt -print-changed`, `mem2reg`
diffs on small programs.
*Read:* LangRef "High Level Structure" only; `Passes.rst` entries for passes you see fire.
*Exit:* narrate what SROA did to `%sum` in `dot.ll`, in IR terms.

### Phase 2 — Touch
Write the walker: parse `dot.ll`, print per-function BB/inst counts + opcode names.
No passes, no pipeline — IR as C++ objects.
*Read:* `llvm/IR/Module.h`, `Function.h`, `BasicBlock.h` — class shapes only.
*Exit:* counts match the text you already read.

### Phase 3 — Drive
Your own driver: `InitLLVM`, `cl::opt`, `TargetMachine` from triple, `PassBuilder` +
4 analysis managers + `crossRegisterProxies` + `parsePassPipeline("default<O2>")` +
`MPM.run()`. Print total insts before/after with your Phase-2 walker.
*Read:* `llvm/tools/opt/NewPMDriver.cpp` (shape only); `PassBuilder.h` ctor + `parsePassPipeline`.
*Exit:* inst count drops — you ran the optimizer programmatically.

### Phase 4 — Listen
Callbacks: before/after names + `chrono` timing. Learn the contract — what fires when,
what skipped means. Don't open the `Any` yet.
*Read:* `llvm/IR/PassInstrumentation.h` (~lines 74-140: Func typedefs + register methods).
*Exit:* pass list matches the reference output.

### Phase 5 — Capture
Unwrap the box: `any_cast<const Function*>` / `Module*`, `raw_string_ostream` + `print()`,
count, stack-pair the nesting. Attempt first, reference after.
*Read:* `llvm/ADT/Any.h` (`any_cast` overloads).
*Exit:* your per-pass before→after table matches the reference exactly.

### Phase 6 — Measure (slow down here — this is the intellectual core)
Design "significant" for real: structural delta, opcode-histogram distance, `TTI`
cost queries — then argue with your own numbers on 2 kernels. Ask what each term means:
removed work? added precision? restructuring that enables later passes? Bonus depth:
query TTI under two triples (X86 vs AArch64) and watch "cost" change meaning.
*Read:* `llvm/Analysis/TargetTransformInfo.h` (`getInstructionCost`).
*Exit:* defend every term in your formula; point at a pass where each term matters.

### Phase 7 — Explain
Terminal view: per-pass table sorted by impact, top-changers, one-line narratives,
zero-delta silence. The terminal IS the view — no HTML, ever.
*Exit:* `./lpta dot.ll` tells the whole story on one screen.

### Phase 8 — Write (the unlocked half of the NPM)
Author a real `FunctionPass` via `PassInfoMixin`: a tiny, honest transform
(e.g., strength-reduce `mul` by power-of-two to `shl`, or fold `add 0` — then check
what InstCombine already does and pick something it doesn't). Get `PreservedAnalyses`
right — that correctness question is the whole lesson. Register it into your pipeline,
run it through YOUR analyzer, read its impact score off YOUR tool. Closed loop.
*Read:* one small pass source, e.g. `llvm/lib/Transforms/Utils/SimplifyCFG.cpp` `run()`
shape; `PreservedAnalyses` docs.
*Exit:* your pass appears in your tool's table with a nonzero, explicable delta.

### Phase 9 — Compare (if hungry)
Same input, two pipelines (`O2` vs `O3`, or `O0` vs `O2`), align by name, flag flips.
*Exit:* point at the pass responsible for an O2-vs-O3 difference on your kernel.

### Phase 10 — Understand (pure learning, no code required)
Take the top 5 passes your tool flags and learn what each actually does from source:
`run()` in `llvm/lib/Transforms/*`, ~100 lines each. Suggested: SROA, EarlyCSE/GVN,
InstCombine, SimplifyCFG, Inliner.
*Exit:* per pass, one paragraph — invariant established, cost, triggering IR pattern.

**Done = Phases 1-8 + 10.** The background build runs in parallel; Phase 9 if hungry. The only
thing still out of scope is a GUI — chrome, not learning.

---

## 6. The Significance Metric (intellectual core)

> ⛔ SEALED until you pass Phase 0's exit test — and re-sealed until Phase 6. "How much did
> it matter" is a question you must answer yourself first. Come back only to compare notes.

Per pass, compute a **Transformation Impact Score** (0–1) from three components:

1. **Structural delta** — functions/BBs/instructions added vs removed, weighted by kind.
2. **Semantic signature distance** — histogram of `(opcode, operand types)` per module; cosine/Hellinger distance between before/after. A pass that only renames things ≈ 0; a pass that deletes 100 instructions scores high.
3. **Cost delta (optional)** — `TargetTransformInfo` estimated instruction cost before/after.

Compose into a single score + a short human-readable "what changed" string, e.g.:
> `Inliner: -128 instructions, 3 callsites inlined, impact 0.71`

Also record pass timing and pass remarks for extra evidence.

**Cross-run regression score (M6):** for two runs of the same input, align passes by name and compute per-pass `|impactA - impactB|`. Flag passes above a threshold as regression suspects, and attribute the end-to-end metric change (e.g., final instruction count) to the highest-delta passes.

---

## 7. Risk Management

| Risk | Mitigation |
|---|---|
| Reading without typing | Type every line; reference only after attempting. Reading pass source (§4A tree) is learning; reading about process is not |
| Stuck on an LLVM API for hours | Bring the exact error + ~20 lines — unblocking is my job |
| Rushing Phase 6 or 10 | These two ARE the depth. Borrow time from Phase 9, never from them |
| Source build eats the machine | X86-only Release is the scoped default; abandon without guilt if it chokes |
| Linking against LLVM fails (ABI/version) | LLVM 22.1.8 is already installed here and the scaffold builds against it; if moving to another distro, keep the same major version |
| Text-diff parsing fragility | We snapshot in-memory IR, never parse text |
| Report looks amateur | M7 dedicates ~10h to HTML/CSS polish; it's your demo face |
| "Show me it's real" | Benchmarks + pass-remarks cross-check in README |

---

## 8. Non-goals (the only remaining scope decisions)

- No GUI, no HTML — the terminal is the view. Presentation effort goes into Phase 7's table, nowhere else.
- No multi-arch backend support — but querying TTI under two triples (Phase 6 bonus) is in, because it teaches what "cost" means.
- No submission artifacts. If a portfolio README happens on its own at the end, fine — it's never a milestone.

## 9. Start now

1. Kick off the Phase 0 source build (§4A) — then ignore it for days.
2. `git init && git add -A && git commit -m "reference (rebuilding from Phase 1)"`.
3. Run `./build/lpta tests/dot.ll` once, save the output — the target your rebuild reproduces by Phase 5.
4. Open Phase 1. Blank file. Go.