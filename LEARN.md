# LEARN.md — Reference only (vocabulary + commands + IR walkthrough)

**Status:** no schedules, no day-by-day, no source guidance — deliberately. This file contains
only three things, none of which reveal any design: words, black-box commands, and how to
read one IR file. If anything here ever tells you *how to build the tool*, that's a bug —
tell me and I'll delete it.

---

## Mini glossary (10 words)

| Word | Means |
|---|---|
| **Module** | The whole .ll file. One compilation unit: functions + globals. |
| **Function** | `define i32 @dot(...) { ... }`. Like a C function. |
| **Basic Block** | A straight-line chunk of instructions with one entry, one exit. Labeled: `entry:`. |
| **Instruction** | One operation: `add`, `load`, `br`, `call`... Everything in a block. |
| **SSA** | Every register (`%x`) is assigned **exactly once**. This is why IR is analyzable. |
| **register** | A `%name` in the text. LLVM "virtual registers" — unlimited, not the CPU's. |
| **Pass** | A program that transforms IR: `mem2reg`, `instcombine`, `gvn`... The "optimizer" is ~150 of these. |
| **Analysis** | A program that *reads* IR without changing it. |
| **Pipeline** | An ordered list of passes, e.g. `default<O2>`. |
| **-print-changed** | LLVM's own debug switch: prints IR after any pass that modified it. |

---

## Black-box commands (running tools teaches; reading source waits)

```bash
# C -> IR at different optimization levels
clang -S -emit-llvm -O0 my.c
clang -S -emit-llvm -O2 my.c

# run one pass and see what it alone does
opt tests/dot.ll -passes='mem2reg' -S
opt tests/dot.ll -passes='gvn' -S

# watch IR evolve pass by pass (LLVM's own transformation history)
opt tests/dot.ll -passes='default<O2>' -print-changed 2>&1 | less

# the sealed reference tool, as a black box: run it, never read it
./build/lpta tests/dot.ll
```

---

## `tests/dot.ll`, line by line (IR reading practice)

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

Exercise: `opt tests/dot.ll -passes='mem2reg' -S`, then diff against the original.
`mem2reg` notices `%sum` is loaded/stored with nothing escaping and promotes it to a
plain register — the `alloca`/`load`/`store` vanish. Ask yourself: `%i` was already a
register (the `phi`) — so which values needed memory at all, and why?

Noticing changes like this one — and asking *how big* each change was — is the heart of
the problem statement. Nothing here says how to automate that. That's Phase 0's job.
