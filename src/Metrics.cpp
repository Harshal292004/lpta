#include "Metrics.h"

#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/Config/llvm-config.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/CFG.h"
#include "llvm/IR/Dominators.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <limits>

namespace lpta {

MetricsDelta delta(const Metrics &Before, const Metrics &After) {
  MetricsDelta D;
  D.insts =
      static_cast<int64_t>(After.insts) - static_cast<int64_t>(Before.insts);
  D.bbs = static_cast<int64_t>(After.bbs) - static_cast<int64_t>(Before.bbs);
  D.functions = static_cast<int64_t>(After.functions) -
                static_cast<int64_t>(Before.functions);
  D.loads =
      static_cast<int64_t>(After.loads) - static_cast<int64_t>(Before.loads);
  D.stores =
      static_cast<int64_t>(After.stores) - static_cast<int64_t>(Before.stores);
  D.calls =
      static_cast<int64_t>(After.calls) - static_cast<int64_t>(Before.calls);
  D.branches = static_cast<int64_t>(After.branches) -
               static_cast<int64_t>(Before.branches);
  D.phis = static_cast<int64_t>(After.phis) - static_cast<int64_t>(Before.phis);
  D.allocas = static_cast<int64_t>(After.allocas) -
              static_cast<int64_t>(Before.allocas);
  D.loops =
      static_cast<int64_t>(After.loops) - static_cast<int64_t>(Before.loops);
  D.maxLoopDepth = static_cast<int64_t>(After.maxLoopDepth) -
                   static_cast<int64_t>(Before.maxLoopDepth);
  D.callEdges = static_cast<int64_t>(After.callEdges) -
                static_cast<int64_t>(Before.callEdges);
  return D;
}

ScoreWeights defaultWeights() { return ScoreWeights(); }

namespace {

bool parseDouble(llvm::StringRef Text, double &Out) {
  if (Text.empty())
    return false;
  std::string S = Text.str();
  char *End = nullptr;
  double Value = std::strtod(S.c_str(), &End);
  if (End == S.c_str() || *End != '\0')
    return false;
  Out = Value;
  return true;
}

} // namespace

bool loadWeightsFromFile(llvm::StringRef Path, ScoreWeights &Weights,
                         std::string &Err) {
  auto BufOrErr = llvm::MemoryBuffer::getFile(Path);
  if (!BufOrErr) {
    Err = "cannot read weights file '" + Path.str() + "'";
    return false;
  }

  unsigned LineNo = 0;
  ScoreWeights Parsed = Weights;
  llvm::SmallVector<llvm::StringRef, 32> Lines;
  (*BufOrErr)->getBuffer().split(Lines, '\n');
  for (llvm::StringRef Line : Lines) {
    ++LineNo;
    Line = Line.split('#').first.trim();
    if (Line.empty())
      continue;

    auto KV = Line.split('=');
    llvm::StringRef Key = KV.first.trim();
    llvm::StringRef Value = KV.second.trim();
    double W = 0;
    if (Key.empty() || Value.empty() || !parseDouble(Value, W) ||
        !std::isfinite(W) || W < 0) {
      Err = "invalid weights line " + std::to_string(LineNo) + ": '" +
            Line.str() + "'";
      return false;
    }

    if (Key == "insts")
      Parsed.insts = W;
    else if (Key == "bbs")
      Parsed.bbs = W;
    else if (Key == "functions")
      Parsed.functions = W;
    else if (Key == "loads")
      Parsed.loads = W;
    else if (Key == "stores")
      Parsed.stores = W;
    else if (Key == "calls")
      Parsed.calls = W;
    else if (Key == "branches")
      Parsed.branches = W;
    else if (Key == "phis")
      Parsed.phis = W;
    else if (Key == "allocas")
      Parsed.allocas = W;
    else if (Key == "loops")
      Parsed.loops = W;
    else if (Key == "max_loop_depth" || Key == "maxLoopDepth")
      Parsed.maxLoopDepth = W;
    else if (Key == "call_edges" || Key == "callEdges")
      Parsed.callEdges = W;
    else
      llvm::errs() << "LPTA warning: unknown weight '" << Key << "' on line "
                   << LineNo << " (ignored)\n";
  }
  Weights = Parsed;
  return true;
}

double significance(const Metrics &Before, const MetricsDelta &D,
                    const ScoreWeights &W) {
  const auto A = [](int64_t V) -> double {
    return static_cast<double>(V < 0 ? -V : V);
  };

  double Sum = 0;
  Sum += W.insts * A(D.insts);
  Sum += W.bbs * A(D.bbs);
  Sum += W.functions * A(D.functions);
  Sum += W.loads * A(D.loads);
  Sum += W.stores * A(D.stores);
  Sum += W.calls * A(D.calls);
  Sum += W.branches * A(D.branches);
  Sum += W.phis * A(D.phis);
  Sum += W.allocas * A(D.allocas);
  Sum += W.loops * A(D.loops);
  Sum += W.maxLoopDepth * A(D.maxLoopDepth);
  Sum += W.callEdges * A(D.callEdges);

  double Denom = static_cast<double>(std::max<uint64_t>(1, Before.insts));
  return Sum / Denom;
}

namespace {

bool isBranchLike(const llvm::Instruction &I) {
#if LLVM_VERSION_MAJOR >= 23
  const unsigned Op = I.getOpcode();
  return Op == llvm::Instruction::UncondBr || Op == llvm::Instruction::CondBr ||
         llvm::isa<llvm::SwitchInst>(I) || llvm::isa<llvm::IndirectBrInst>(I) ||
         llvm::isa<llvm::CallBrInst>(I);
#else
  return llvm::isa<llvm::BranchInst>(I) || llvm::isa<llvm::SwitchInst>(I) ||
         llvm::isa<llvm::IndirectBrInst>(I) || llvm::isa<llvm::CallBrInst>(I);
#endif
}

void countInstructions(llvm::Function &F, Metrics &M,
                       llvm::SmallPtrSetImpl<const llvm::Function *> &Callees) {
  for (llvm::BasicBlock &BB : F) {
    ++M.bbs;
    for (llvm::Instruction &I : BB) {
      ++M.insts;
      if (llvm::isa<llvm::LoadInst>(&I))
        ++M.loads;
      else if (llvm::isa<llvm::StoreInst>(&I))
        ++M.stores;
      else if (llvm::isa<llvm::PHINode>(&I))
        ++M.phis;
      else if (llvm::isa<llvm::AllocaInst>(&I))
        ++M.allocas;

      if (auto *CB = llvm::dyn_cast<llvm::CallBase>(&I)) {
        ++M.calls;
        if (llvm::Function *Callee = CB->getCalledFunction())
          Callees.insert(Callee);
      }

      if (isBranchLike(I))
        ++M.branches;
    }
  }
  M.callEdges = Callees.size();
}

void countLoops(llvm::Function &F, Metrics &M) {
  llvm::DominatorTree DT(F);
  llvm::LoopInfo LI(DT);
  llvm::SmallVector<llvm::Loop *, 8> Loops = LI.getLoopsInPreorder();
  M.loops = Loops.size();
  for (llvm::Loop *L : Loops)
    M.maxLoopDepth = std::max<uint64_t>(M.maxLoopDepth, L->getLoopDepth());
}

void collectLoopBlocks(
    const llvm::Loop &L,
    llvm::SmallPtrSetImpl<const llvm::BasicBlock *> &Blocks) {
  for (const llvm::BasicBlock *BB : L.blocks())
    Blocks.insert(BB);
  for (const llvm::Loop *Sub : L.getSubLoops())
    collectLoopBlocks(*Sub, Blocks);
}

void countLoopTree(const llvm::Loop &L, uint64_t &Loops, uint64_t &MaxDepth) {
  ++Loops;
  MaxDepth = std::max<uint64_t>(MaxDepth, L.getLoopDepth());
  for (const llvm::Loop *Sub : L.getSubLoops())
    countLoopTree(*Sub, Loops, MaxDepth);
}

} // namespace

Metrics computeFunctionMetrics(const llvm::Function &F) {
  Metrics M;
  M.functions = 1;
  if (F.isDeclaration())
    return M;

  llvm::Function &MutF = const_cast<llvm::Function &>(F);
  llvm::SmallPtrSet<const llvm::Function *, 8> Callees;
  countInstructions(MutF, M, Callees);
  countLoops(MutF, M);
  return M;
}

Metrics computeModuleMetrics(const llvm::Module &M,
                             std::map<std::string, Metrics> *ByFunction) {
  Metrics Total;
  for (const llvm::Function &F : M) {
    if (F.isDeclaration())
      continue;
    Metrics FM = computeFunctionMetrics(F);
    Total.insts += FM.insts;
    Total.bbs += FM.bbs;
    Total.functions += 1;
    Total.loads += FM.loads;
    Total.stores += FM.stores;
    Total.calls += FM.calls;
    Total.branches += FM.branches;
    Total.phis += FM.phis;
    Total.allocas += FM.allocas;
    Total.loops += FM.loops;
    Total.maxLoopDepth = std::max(Total.maxLoopDepth, FM.maxLoopDepth);
    Total.callEdges += FM.callEdges;
    if (ByFunction)
      (*ByFunction)[F.getName().str()] = FM;
  }
  return Total;
}

Metrics computeLoopMetrics(const llvm::Loop &L) {
  Metrics M;
  M.functions = 1;

  llvm::SmallPtrSet<const llvm::BasicBlock *, 32> Blocks;
  collectLoopBlocks(L, Blocks);

  llvm::SmallPtrSet<const llvm::Function *, 8> Callees;
  for (const llvm::BasicBlock *BB : Blocks) {
    ++M.bbs;
    for (const llvm::Instruction &I : *BB) {
      ++M.insts;
      if (llvm::isa<llvm::LoadInst>(&I))
        ++M.loads;
      else if (llvm::isa<llvm::StoreInst>(&I))
        ++M.stores;
      else if (llvm::isa<llvm::PHINode>(&I))
        ++M.phis;
      else if (llvm::isa<llvm::AllocaInst>(&I))
        ++M.allocas;

      if (const auto *CB = llvm::dyn_cast<llvm::CallBase>(&I)) {
        ++M.calls;
        if (const llvm::Function *Callee = CB->getCalledFunction())
          Callees.insert(Callee);
      }

      if (isBranchLike(I))
        ++M.branches;
    }
  }
  M.callEdges = Callees.size();
  countLoopTree(L, M.loops, M.maxLoopDepth);
  return M;
}

void printMetrics(llvm::raw_ostream &OS, const Metrics &M) {
  OS << "insts=" << M.insts << " bbs=" << M.bbs << " funcs=" << M.functions
     << " loads=" << M.loads << " stores=" << M.stores << " calls=" << M.calls
     << " branches=" << M.branches << " phis=" << M.phis
     << " allocas=" << M.allocas << " loops=" << M.loops
     << " depth=" << M.maxLoopDepth << " edges=" << M.callEdges;
}

void printDelta(llvm::raw_ostream &OS, const MetricsDelta &D) {
  OS << "insts=" << D.insts << " bbs=" << D.bbs << " calls=" << D.calls
     << " loads=" << D.loads << " stores=" << D.stores;
}

} // namespace lpta
