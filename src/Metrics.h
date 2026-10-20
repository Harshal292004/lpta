#ifndef LPTA_METRICS_H
#define LPTA_METRICS_H

#include "llvm/ADT/StringRef.h"

#include <cstdint>
#include <map>
#include <string>

namespace llvm {
class Function;
class Loop;
class Module;
class raw_ostream;
} // namespace llvm

namespace lpta {

/// Structural metrics collected from one IR unit.  All fields are counts, so
/// they are cheap to compare and easy to expose in a trace.
struct Metrics {
  uint64_t insts = 0;
  uint64_t bbs = 0;
  uint64_t functions = 0;
  uint64_t loads = 0;
  uint64_t stores = 0;
  uint64_t calls = 0;
  uint64_t branches = 0;
  uint64_t phis = 0;
  uint64_t allocas = 0;
  uint64_t loops = 0;
  uint64_t maxLoopDepth = 0;
  uint64_t callEdges = 0;
};

struct MetricsDelta {
  int64_t insts = 0;
  int64_t bbs = 0;
  int64_t functions = 0;
  int64_t loads = 0;
  int64_t stores = 0;
  int64_t calls = 0;
  int64_t branches = 0;
  int64_t phis = 0;
  int64_t allocas = 0;
  int64_t loops = 0;
  int64_t maxLoopDepth = 0;
  int64_t callEdges = 0;
};

MetricsDelta delta(const Metrics &before, const Metrics &after);

/// Weights used by the significance score.  The score is deliberately a
/// heuristic: it measures the magnitude of a change, not its runtime effect.
struct ScoreWeights {
  double insts = 1.0;
  double bbs = 0.5;
  double functions = 0.25;
  double loads = 1.0;
  double stores = 1.0;
  double calls = 3.0;
  double branches = 0.5;
  double phis = 0.5;
  double allocas = 2.0;
  double loops = 5.0;
  double maxLoopDepth = 5.0;
  double callEdges = 2.0;
};

ScoreWeights defaultWeights();

/// Load "name=value" weights from a file.  Missing keys keep their defaults.
/// Returns false and fills Err on failure.
bool loadWeightsFromFile(llvm::StringRef Path, ScoreWeights &Weights,
                         std::string &Err);

/// score = sum(w_m * |after_m - before_m|) / max(1, before.insts).
double significance(const Metrics &Before, const MetricsDelta &Delta,
                    const ScoreWeights &Weights);

Metrics computeFunctionMetrics(const llvm::Function &F);
/// Metrics for a module.  When ByFunction is non-null, per-function metrics are
/// filled in as well (including functions that are declarations? no: only
/// definitions, which are the units the optimizer can change).
Metrics computeModuleMetrics(const llvm::Module &M,
                             std::map<std::string, Metrics> *ByFunction);

/// Metrics for the region covered by a loop, including nested loops.
Metrics computeLoopMetrics(const llvm::Loop &L);

void printMetrics(llvm::raw_ostream &OS, const Metrics &M);
void printDelta(llvm::raw_ostream &OS, const MetricsDelta &D);

} // namespace lpta

#endif
