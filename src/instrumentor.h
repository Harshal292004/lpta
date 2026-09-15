#ifndef LPTA_INSTRUMENTOR_H
#define LPTA_INSTRUMENTOR_H

#include "llvm/ADT/Any.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/IR/PassInstrumentation.h"
#include "llvm/Passes/PassBuilder.h"

#include <chrono>
#include <string>
#include <vector>

namespace lpta {

// IR state captured at one instrumentation point.
struct Snapshot {
  std::string IR; // serialized textual IR of the unit the pass ran on
  int Insts = 0;  // total instructions in the unit
  int BBs = 0;    // total basic blocks in the unit
  int Funcs = 0;  // functions covered (1 for a Function unit)
  bool Empty = true;
};

// One pass invocation: what ran, how long it took, IR before vs after.
struct PassRecord {
  std::string Name;
  double ElapsedMs = 0.0;
  Snapshot Before;
  Snapshot After;

  int instDelta() const { return After.Insts - Before.Insts; }
  int bbDelta() const { return After.BBs - Before.BBs; }
};

class PipelineInstrumentor {
public:
  llvm::PassInstrumentationCallbacks &callbacks() { return PIC; }

  void registerCallbacks(llvm::PassBuilder &PB);

  const std::vector<PassRecord> &records() const { return Records; }

  void printSummary() const;

private:
  struct Pending {
    std::string Name;
    std::chrono::steady_clock::time_point Start;
    Snapshot Before;
  };

  // Unwrap LLVM's type-erased IR handle into a counted + serialized snapshot.
  // Handles Function and Module units; anything else yields an empty snapshot
  // (name + timing are still recorded).
  static Snapshot takeSnapshot(const llvm::Any &IR);

  llvm::PassInstrumentationCallbacks PIC;
  std::vector<PassRecord> Records;
  std::vector<Pending> Stack; // pairs before/after across nested pass managers
};

} // namespace lpta

#endif