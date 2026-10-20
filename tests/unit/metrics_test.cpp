#include "Diff.h"
#include "Metrics.h"

#include "llvm/ADT/SmallVector.h"
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/AsmParser/Parser.h"
#include "llvm/IR/Dominators.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/SourceMgr.h"
#include "llvm/Support/raw_ostream.h"

#include <string>

using namespace llvm;

static int Failures = 0;

static void check(bool Cond, const char *What) {
  if (!Cond) {
    errs() << "FAIL: " << What << "\n";
    ++Failures;
  }
}

static void checkEq(uint64_t Got, uint64_t Want, const char *What) {
  if (Got != Want) {
    errs() << "FAIL: " << What << " (got " << Got << ", want " << Want << ")\n";
    ++Failures;
  }
}

int main() {
  LLVMContext Ctx;
  SMDiagnostic Err;
  const char *IR = R"IR(
    declare i32 @callee(i32)

    define i32 @loops(i32 %x) {
    entry:
      %slot = alloca i32, align 4
      store i32 %x, ptr %slot, align 4
      %v = load i32, ptr %slot, align 4
      %c = call i32 @callee(i32 %v)
      br label %outer

    outer:
      %i = phi i32 [ 0, %entry ], [ %i.next, %outer.latch ]
      %i.next = add i32 %i, 1
      br label %inner

    inner:
      %j = phi i32 [ 0, %outer ], [ %j.next, %inner ]
      %j.next = add i32 %j, 1
      %cond = icmp slt i32 %j.next, 10
      br i1 %cond, label %inner, label %outer.latch

    outer.latch:
      %done = icmp eq i32 %i.next, 4
      br i1 %done, label %exit, label %outer

    exit:
      ret i32 %c
    }
  )IR";

  std::unique_ptr<Module> M = parseAssemblyString(IR, Err, Ctx);
  if (!M) {
    Err.print("metrics-test", errs());
    return 1;
  }

  Function *F = M->getFunction("loops");
  check(F != nullptr, "function parsed");
  if (F) {
    lpta::Metrics FM = lpta::computeFunctionMetrics(*F);
    checkEq(FM.insts, 15, "function instructions");
    checkEq(FM.bbs, 5, "function basic blocks");
    checkEq(FM.loads, 1, "function loads");
    checkEq(FM.stores, 1, "function stores");
    checkEq(FM.calls, 1, "function calls");
    checkEq(FM.branches, 4, "function branches");
    checkEq(FM.phis, 2, "function phis");
    checkEq(FM.allocas, 1, "function allocas");
    checkEq(FM.loops, 2, "function loops");
    checkEq(FM.maxLoopDepth, 2, "function max loop depth");
    checkEq(FM.callEdges, 1, "function call edges");
    checkEq(FM.functions, 1, "function count");

    DominatorTree DT(*F);
    LoopInfo LI(DT);
    llvm::SmallVector<llvm::Loop *, 8> Loops = LI.getLoopsInPreorder();
    checkEq(Loops.size(), 2, "loop count");
    if (Loops.size() == 2) {
      lpta::Metrics LM = lpta::computeLoopMetrics(*Loops[0]);
      checkEq(LM.insts, 9, "outer loop instructions");
      checkEq(LM.bbs, 3, "outer loop basic blocks");
      checkEq(LM.branches, 3, "outer loop branches");
      checkEq(LM.phis, 2, "outer loop phis");
      checkEq(LM.loops, 2, "outer loop nested loop count");
      checkEq(LM.maxLoopDepth, 2, "outer loop depth");
      checkEq(LM.calls, 0, "outer loop calls");
    }

    lpta::MetricsDelta D;
    D.insts = -2;
    D.calls = 1;
    lpta::ScoreWeights W = lpta::defaultWeights();
    double Score = lpta::significance(FM, D, W);
    check(Score > 0, "significance is positive for a change");
    checkEq((uint64_t)(Score * 1000),
            (uint64_t)((2.0 * 1.0 + 1.0 * 3.0) /
                       std::max<uint64_t>(1, FM.insts) * 1000),
            "significance formula");
  }

  lpta::Metrics MM = lpta::computeModuleMetrics(*M, nullptr);
  checkEq(MM.functions, 1, "module function count");
  checkEq(MM.insts, 15, "module instructions");
  checkEq(MM.callEdges, 1, "module call edges");

  std::string Diff = lpta::unifiedDiff("a\nb\nc\n", "a\nB\nc\n", "test");
  check(Diff.find("--- before/test") != std::string::npos, "diff header");
  check(Diff.find("-b") != std::string::npos, "diff deletion");
  check(Diff.find("+B") != std::string::npos, "diff insertion");

  std::string Same = lpta::unifiedDiff("a\nb\n", "a\nb\n", "test");
  check(Same.empty(), "identical inputs produce no diff");

  if (Failures == 0)
    outs() << "lpta-metrics-test: PASS\n";
  return Failures == 0 ? 0 : 1;
}
