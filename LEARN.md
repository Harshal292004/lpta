# LEARN.md — Reference (IR concepts; schedule retired)

**Status:** you read IR fluently, so the day-by-day schedule below is retired — PLAN.md §5
(the 10-day rebuild) is the plan now. Keep this file for its lasting parts: the glossary,
the golden commands, and the `dot.ll` line-by-line walkthrough (useful on rebuild Day 1).
Ignore all dates and "Day N" framing below.

---

You have ~1-2h/day this week. This guide is built around one rule:

> **Read OUTPUT, not source.** Every day you run commands and read the IR that comes out.
> You only open C++ when the concepts behind it are already obvious to you.

By the evening of 7 Sept, `src/main.cpp` will make sense; by the morning of 8 Sept you'll
have modified the tool yourself. You are NOT expected to understand any of it right now. Today you learn what IR is.

---

## Mini glossary (memorize these 10 words, in this order)

| Word | Means |
|---|---|
| **Module** | The whole .ll file. One compilation unit: functions + globals. |
| **Function** | `define i32 @dot(...) { ... }`. Like a C function. |
| **Basic Block** | A straight-line chunk of instructions with one entry, one exit. Labeled: `entry:`. |
| **Instruction** | One operation: `add`, `load`, `br`, `call`... Everything in a block. |
| **SSA** | Every register (`%x`) is assigned **exactly once**. This is the secret of why IR is easy to optimize. |
| **register** | A `%name` in the text. LLVM "virtual registers" — unlimited, not the CPU's. |
| **Pass** | A program that transforms IR: `mem2reg`, `instcombine`, `gvn`... The "optimizer" is ~150 of these. |
| **Analysis** | A program that *reads* IR without changing it (e.g., "how many instructions are here?"). |
| **Pipeline** | An ordered list of passes, e.g. `default<O2>`. |
| **-print-changed** | LLVM's own debug switch that prints IR after any pass that modified it. |

---

## Golden commands (use these every single day)

```bash
# C -> IR at different optimization levels
clang -S -emit-llvm -O0 my.c          # unoptimized IR, easy to read
clang -S -emit-llvm -O2 my.c          # optimized IR

# run one pass and see what it alone does
opt tests/dot.ll -passes='mem2reg' -S
opt tests/dot.ll -passes='gvn' -S

# see the IR evolve pass by pass (LLVM's own "transformation history")
opt tests/dot.ll -passes='default<O2>' -print-changed 2>&1 | less

# OUR TOOL: every pass that ran + how long it took
./build/lpta tests/dot.ll
```

---

## Day-by-day

### Day 1 — Meet the IR (~90 min)
Do NOT touch the repo's code. Make a scratch folder:

```bash
mkdir ~/labs && cd ~/labs
echo 'int sum(int n){ int s=0; for(int i=0;i<n;i++) s+=i; return s; }' > s.c
clang -S -emit-llvm -O0 s.c && cat s.ll
```

- Read `s.ll` out loud, line by line. Find: the module, the function, the blocks, a `load`, a `store`, a `br`, a `phi`.
- Now `clang -S -emit-llvm -O2 s.c` and eyeball the difference. The optimized version is much shorter.
- **Done when:** you can point at any line and say *"this computes X"*.

Open ONLY this: `https://llvm.org/docs/LangRef.html` — read the first ~150 lines
("High Level Structure"), then close it. Do not read the whole thing. Ever.

### Day 2 — My `tests/dot.ll`, line by line (~90 min)
Open `tests/dot.ll`. Walk it with this map:

```
define i32 @dot(ptr %a, ptr %b, i32 %n) {
  %sum = alloca i32              ; "int sum" lived in memory at -O0. Pointer in %sum.
  store i32 0, ptr %sum          ; sum = 0
  br label %header

header:
  %i = phi i32 [ 0, %entry ], [ %inc, %body ]  ; loop counter: entry gives 0, body gives %inc
  %cmp = icmp slt i32 %i, %n
  br i1 %cmp, label %body, label %exit

body:
  %ai = getelementptr i32, ptr %a, i32 %i   ; address of a[i]  (= a + i)
  %av = load i32, ptr %ai                    ; a[i]
  %bi = getelementptr i32, ptr %b, i32 %i
  %bv = load i32, ptr %bi                    ; b[i]
  %mul = mul i32 %av, %bv                    ; a[i]*b[i]
  %cur = load i32, ptr %sum
  %next = add i32 %cur, %mul
  store i32 %next, ptr %sum
  %inc = add i32 %i, 1
  br label %header

exit:
  %result = load i32, ptr %sum
  ret i32 %result
}
```

Now run `opt tests/dot.ll -passes='mem2reg' -S` and diff against the original:
`diff <(opt tests/dot.ll -passes='mem2reg' -S) tests/dot.ll`

- **The punchline:** `mem2reg` noticed `%sum` is loaded/stored with nothing escaping
  and promoted it to a plain register — deleting the `alloca`/`load`/`store`.
  THIS is exactly the kind of thing our tool will *measure*.
- **Done when:** you can explain why `mem2reg` would care about `%sum` and `%i` (hint: `%i` is already a register — the `phi`).

### Day 3 — The pipeline: who runs, and why (~90 min)
```bash
./build/lpta tests/dot.ll          # our tool: watch the 153 passes roll by
opt tests/dot.ll -passes='default<O2>' -print-changed 2>&1 | less
```
- Watch the IR mutate live in `less`. Press `j`/`k` to scroll. This is a real transformation history — the thing you're building an automated version of.
- Now open `https://llvm.org/docs/Passes.html` and look up the FIRST 15 pass names you saw. That's it — 15 names, one line each.
- **Done when:** you can say why `SimplifyCFG`, `SROA`/`Promote`, `InstCombine`, `GVN`, and `Inliner` exist in that order (roughly: tidy up → promote memory → local simplifications → global value reuse → merge functions).

### Day 4 — The Pass Manager: `main.cpp` stops being scary (~90 min)
Concepts first (memorize this mental model):
> **You hand LLVM a Module and a pipeline string. LLVM runs the passes. Your code just
> listens.** That's the entire tool. Nothing more.

Now open `src/main.cpp` and map each block:

| Lines | What it does | Why |
|---|---|---|
| `InitializeAllTargets...` | Loads x86/arm backend knowledge | Passes need to know the CPU for cost decisions |
| `parseIRFile(...)` | Reads `dot.ll` into an in-memory `Module` | This is the object every pass operates on |
| `createTargetMachine` | The CPU description | e.g. "is this x86-64?" |
| `PassBuilder PB(...)` | Factory that can build ANY pipeline | This is the core of driving LLVM programmatically |
| 4 analysis managers | Caches for pass queries | Passes ask "what's the loop depth here?" |
| `parsePassPipeline(...)` | Turns the string `"default<O2>"` into a real pass list | |
| `MPM.run(*M, MAM)` | **Run everything.** | Inside here is where our callbacks fire |

- Also skim `llvm/tools/opt/NewPMDriver.cpp` for 10 minutes — just find the same 4 or 5 blocks you just read. You're reading the reference implementation; don't understand every line, just recognize the shape.
- **Done when:** you can explain `main.cpp` in 5 spoken sentences to nobody in particular. Say them out loud. Then move on.

### Day 5 — The callbacks: `instrumentor.cpp` (~90 min)
Concepts first:
> A **callback** is a function you *give* to LLVM. LLVM promises to call it for you:
> once right before a pass runs, once right after. That's the "before/after snapshot" hook.

Open `src/instrumentor.cpp`. You'll see:
- `registerBeforeNonSkippedPassCallback(...)` — fires when a pass is about to run. We record a timestamp and print `[run] Name`.
- `registerAfterPassCallback(...)` — fires after it ran. We print the name + elapsed ms.

Now TOUCH the code. Do this and rebuild:

```cpp
// inside the after-callback, change the print line to:
llvm::errs() << "  DONE " << Name << "\n";
```
```bash
cmake --build build && ./build/lpta tests/dot.ll | head -20
```
You just modified LLVM infrastructure and it worked. This is the moment the project stops being magic.

- **Done when:** you changed the output text yourself, rebuilt, and saw your change. (This is literally the entire M1 pattern: replace "print a name" with "save the IR".)

### Day 6 — Touch the IR yourself (~90 min)
Real passes manipulate `Function`/`BasicBlock`/`Instruction` objects. Try the read-only version: inside the **after** callback, pull the function out of the snapshot and count its instructions.

The after-callback gives you a `llvm::Any` containing a pointer to the function/module that was just transformed. Add this to `instrumentor.cpp`:

```cpp
llvm::Any &IR = /* the Any passed to your after-callback */;
if (auto *Fn = llvm::any_cast<const llvm::Function *>(&IR)) {
  int count = 0;
  for (const llvm::BasicBlock &BB : **Fn)
    for (const llvm::Instruction &I : BB)
      count++;
  llvm::errs() << "  instructions: " << count << "\n";
}
```
> `any_cast` = "unwrap the box LLVM gave us". `for (BB : Fn)` = walk blocks, then instructions.
> This is the exact same loop M2 (the diff engine) will use to count things.

You may have to fix compile errors — that's the learning. Read the error, read the header
`llvm/include/llvm/IR/Function.h` / `BasicBlock.h` for the type names, fix, rebuild.
- **Done when:** your tool prints an instruction count after every pass.

### Day 7 — Connect the dots (~60 min)
1. Re-read `PLAN.md` §3 (architecture diagram). Every box should now be familiar.
2. `git init && git add -A && git commit -m "scaffold: instrumented O2 harness (LLVM 22)"`
3. Write down, in your own words, three sentences that answer:
   *"How does the tool record IR before and after each pass?"*
   If you can write that, the interview is over — you're ready for Phase 2.

---

## What to IGNORE (you will be tempted, don't)

- ❌ Building LLVM from source. Never.
- ❌ The rest of `llvm/include/llvm/*` — only these 5 files exist this week:
  `IR/Module.h`, `IR/Function.h`, `IR/BasicBlock.h`, `IR/PassInstrumentation.h`,
  `Passes/PassBuilder.h` — and you only *skim* those.
- ❌ `opt`'s source beyond NewPMDriver.cpp.
- ❌ 20-hour "compiler from scratch" YouTube playlists.
- ❌ Reading LangRef fully.

## If you have only 30 minutes on a day

Day 1: just the `clang -S -emit-llvm` commands, read output.
Day 2: just `opt tests/dot.ll -passes='mem2reg' -S` + the diff.
Day 3: just `./build/lpta tests/dot.ll | head -30`.
Day 4: just the mental model paragraph + skim main.cpp for 10 min.
Day 5: just the one-line edit + rebuild.
Day 6: skip (come back in Phase 2).
Day 7: the three sentences. Always do the three sentences.

---

## After 8 Sept (build starts, 4-7h/day)

The curriculum changes gears: you stop learning and start building M1 (snapshot IR to a string
before/after each pass). Knowledge is then *pulled* by the build, not pushed by a guide.
Re-read `PLAN.md` §5 for the day-by-day dates and §4 "Embedded Track" for what each milestone will teach you.

The Deep Track (Cornell CS 6120, Cooper & Torczon, Lattner's talks) is for whenever you're
curious — never an obligation, never in this week.