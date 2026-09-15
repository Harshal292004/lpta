#include "instrumentor.h"

#include "llvm/ADT/Any.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/PassInstrumentation.h"
#include "llvm/Support/raw_ostream.h"

namespace lpta {

Snapshot PipelineInstrumentor::takeSnapshot(const llvm::Any &IR) {
  Snapshot S;
  // LLVM wraps the IR unit as a pointer-to-const (see
  // PassInstrumentation::runBeforePass/runAfterPass), so the cast target must
  // be const-qualified to match the stored type exactly.
  if (auto *FnPtr = llvm::any_cast<const llvm::Function *>(&IR)) {
    const llvm::Function &F = **FnPtr;
    for (const llvm::BasicBlock &BB : F) {
      S.BBs++;
      S.Insts += static_cast<int>(BB.size());
    }
    S.Funcs = 1;
    std::string Str;
    llvm::raw_string_ostream OS(Str);
    F.print(OS);
    OS.flush();
    S.IR = std::move(Str);
    S.Empty = false;
  } else if (auto *ModPtr = llvm::any_cast<const llvm::Module *>(&IR)) {
    const llvm::Module &M = **ModPtr;
    for (const llvm::Function &F : M) {
      if (F.isDeclaration())
        continue;
      S.Funcs++;
      for (const llvm::BasicBlock &BB : F) {
        S.BBs++;
        S.Insts += static_cast<int>(BB.size());
      }
    }
    std::string Str;
    llvm::raw_string_ostream OS(Str);
    M.print(OS, nullptr);
    OS.flush();
    S.IR = std::move(Str);
    S.Empty = false;
  }
  return S;
}

void PipelineInstrumentor::registerCallbacks(llvm::PassBuilder &PB) {
  PIC.registerBeforeNonSkippedPassCallback(
      [this](llvm::StringRef Name, llvm::Any IR) {
        Pending P;
        P.Name = Name.str();
        P.Start = std::chrono::steady_clock::now();
        P.Before = takeSnapshot(IR);
        Stack.push_back(std::move(P));
      });

  PIC.registerAfterPassCallback(
      [this](llvm::StringRef Name, llvm::Any IR,
             const llvm::PreservedAnalyses &) {
        // Pair with the matching before-callback via stack discipline:
        // pass managers nest, so the most recent unmatched before-callback
        // is always the one finishing now.
        if (Stack.empty())
          return;
        Pending P = std::move(Stack.back());
        Stack.pop_back();

        PassRecord R;
        R.Name = P.Name;
        R.ElapsedMs =
            std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - P.Start)
                .count();
        R.Before = std::move(P.Before);
        R.After = takeSnapshot(IR);
        Records.push_back(std::move(R));
      });
}

void PipelineInstrumentor::printSummary() const {
  llvm::outs() << "\n"
               << "pass | ms | insts before -> after (delta) | bbs before -> "
                  "after (delta)\n";
  for (const PassRecord &R : Records) {
    llvm::outs() << R.Name << " | " << R.ElapsedMs << " | " << R.Before.Insts
                 << " -> " << R.After.Insts << " (" << R.instDelta() << ") | "
                 << R.Before.BBs << " -> " << R.After.BBs << " ("
                 << R.bbDelta() << ")\n";
  }
  llvm::outs() << "\nTotal passes run: " << Records.size() << "\n";
}

} // namespace lpta