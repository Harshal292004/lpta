#ifndef LPTA_REPORT_H
#define LPTA_REPORT_H

#include "Metrics.h"

#include "llvm/ADT/StringRef.h"
#include "llvm/Support/Regex.h"

#include <cstdint>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace llvm {
class raw_ostream;
namespace json {
class OStream;
} // namespace json
} // namespace llvm

namespace lpta {

struct NameFilter {
  std::string Text;
  std::shared_ptr<llvm::Regex> RE;

  NameFilter() = default;
  explicit NameFilter(llvm::StringRef Pattern);
  bool empty() const { return Text.empty(); }
  bool matches(llvm::StringRef Name) const;
};

struct ReportOptions {
  std::string view = "summary";
  unsigned top = 10;
  std::string functionFilter;
  std::string passFilter;
  double minScore = 0.0;
  bool diff = false;
  std::string color = "auto";
  std::string out;
  std::string html;
  std::string json;
  std::string compare;
  std::string weights;
  std::string demangle = "auto";
  std::string pipeline;
  bool skipped = false;

  NameFilter functionMatcher;
  NameFilter passMatcher;
  ScoreWeights scoreWeights = defaultWeights();

  bool reportEnabled() const { return view != "off"; }
  bool retainEvents() const;
  bool snapshotsEnabled() const { return diff || !html.empty(); }
  void compileFilters();
};

struct FunctionRecord {
  Metrics before;
  Metrics after;
  bool hasDelta = false;
  MetricsDelta storedDelta;

  MetricsDelta effectiveDelta() const;
};

struct Event {
  uint64_t index = 0;
  std::string pass;
  std::string unitKind;
  std::string unitName;
  std::string unitDisplayName;
  std::vector<std::string> affectedFunctions;
  std::map<std::string, FunctionRecord> functions;
  std::string parentPass;
  unsigned depth = 0;
  bool changed = false;
  bool skipped = false;
  bool invalidated = false;
  uint64_t durationUs = 0;
  Metrics before;
  Metrics after;
  MetricsDelta delta;
  double score = 0.0;
  std::string diff;
  std::string diffRef;
};

struct TraceHeader {
  std::string schemaVersion;
  std::string llvmVersion;
  std::string producer;
  std::string route;
  std::string pipeline;
  std::string source;
  std::string targetTriple;
  std::string lptaVersion;
};

struct Trace {
  TraceHeader header;
  std::vector<Event> events;
};

struct PassStats {
  std::string name;
  uint64_t runs = 0;
  uint64_t changed = 0;
  MetricsDelta delta;
  uint64_t durationUs = 0;
  double score = 0.0;
};

struct FunctionStats {
  std::string name;
  std::string displayName;
  bool haveInsts = false;
  uint64_t firstInsts = 0;
  uint64_t lastInsts = 0;
  uint64_t totalAbsInsts = 0;
  double totalChange = 0.0;
  double totalScore = 0.0;
  uint64_t changedPasses = 0;
  std::map<std::string, double> passContribution;
};

struct AggregateStats {
  uint64_t passesRun = 0;
  uint64_t changedPasses = 0;
  uint64_t functionsTouched = 0;
  uint64_t totalTimeUs = 0;
  uint64_t overheadUs = 0;
  std::set<std::string> functionNames;
  std::map<std::string, PassStats> passes;
  std::map<std::string, FunctionStats> functions;
};

class Aggregator {
public:
  explicit Aggregator(const ReportOptions &Opts) : Opts(Opts) {}

  void add(const Event &E);
  void addOverhead(uint64_t Us) { Stats.overheadUs += Us; }
  void setOverhead(uint64_t Us) { Stats.overheadUs = Us; }

  const AggregateStats &stats() const { return Stats; }
  const std::vector<Event> &retainedEvents() const { return Events; }
  std::vector<Event> &retainedEvents() { return Events; }

private:
  const ReportOptions &Opts;
  AggregateStats Stats;
  std::vector<Event> Events;
};

/// True when the event passes the pass/function/min-score filters.
bool eventMatches(const Event &E, const ReportOptions &Opts);

std::string demangleName(llvm::StringRef Raw);
std::string displayName(llvm::StringRef Raw, llvm::StringRef DemangleMode);
std::string prettyPassName(llvm::StringRef Name);
std::string truncate(llvm::StringRef S, size_t Width);

bool eventHasFunction(const Event &E, llvm::StringRef Function);

std::string renderTerminal(const TraceHeader &Header, const Aggregator &Agg,
                           const ReportOptions &Opts,
                           llvm::StringRef CompareText = {});
std::string renderHtml(const TraceHeader &Header, const Aggregator &Agg,
                       const ReportOptions &Opts);

/// Compare two traces by (function, pass, occurrence index).  The returned
/// text describes the first divergence and the per-pass metric differences.
std::string compareTraces(const Trace &Base, const Trace &Current,
                          unsigned Top);

/// Atomic write of a complete text file.
bool writeFileAtomic(llvm::StringRef Path, llvm::StringRef Contents,
                     std::string &Err);

bool isColorEnabled(llvm::StringRef Mode, bool WritingToTerminal,
                    bool NoColorEnv);

} // namespace lpta

#endif
