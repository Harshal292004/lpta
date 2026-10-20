#include "Collector.h"

#include "Diff.h"
#include "Options.h"
#include "Report.h"
#include "Trace.h"
#include "Version.h"

#include "llvm/ADT/Any.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/Analysis/LazyCallGraph.h"
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/PassInstrumentation.h"
#include "llvm/IR/PassManager.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <vector>

namespace lpta {

namespace {

using Clock = std::chrono::steady_clock;

struct Snapshot {
  bool valid = false;
  std::string kind;
  std::string name;
  std::string display;
  std::vector<std::string> affected;
  Metrics metrics;
  std::map<std::string, Metrics> functions;
  std::string ir;
};

struct Frame {
  std::string pass;
  bool infra = false;
  bool recording = false;
  Clock::time_point start;
  uint64_t childNs = 0;
  Snapshot before;
  std::string parent;
  unsigned depth = 0;
};

thread_local std::vector<Frame> Frames;

bool isInfrastructure(llvm::StringRef Pass) {
  // Pass managers, adaptors, and analysis bookkeeping passes do not transform
  // IR themselves; their child invocations are recorded instead.  Filtering
  // them keeps the report aligned with what `-print-changed` considers a
  // transformation pass and avoids counting wrapper "changes" twice.
  if (Pass.contains("PassManager<") || Pass.ends_with("PassAdaptor"))
    return true;
  if (Pass.starts_with("InvalidateAnalysisPass<") ||
      Pass.starts_with("RequireAnalysisPass<"))
    return true;
  return Pass.ends_with("RepeatedPass") || Pass.ends_with("WrapperPass");
}

std::string printModule(const llvm::Module &M) {
  std::string S;
  llvm::raw_string_ostream OS(S);
  M.print(OS, nullptr);
  return OS.str();
}

std::string printFunction(const llvm::Function &F) {
  std::string S;
  llvm::raw_string_ostream OS(S);
  F.print(OS);
  return OS.str();
}

std::string printLoop(const llvm::Loop &L) {
  std::string S;
  llvm::raw_string_ostream OS(S);
  L.print(OS);
  return OS.str();
}

std::string printSCC(const llvm::LazyCallGraph::SCC &C) {
  std::string S;
  llvm::raw_string_ostream OS(S);
  for (const llvm::LazyCallGraph::Node &N : C)
    N.getFunction().print(OS);
  return OS.str();
}

template <typename T> const T *probeAny(const llvm::Any &A) {
  if (const auto *P = llvm::any_cast<const T *>(&A))
    return *P;
  return nullptr;
}

const llvm::Module *moduleFromAny(llvm::Any &IR) {
  if (const auto *M = probeAny<llvm::Module>(IR))
    return M;
  if (const auto *F = probeAny<llvm::Function>(IR))
    return F->getParent();
  if (const auto *L = probeAny<llvm::Loop>(IR))
    return L->getHeader()->getModule();
  if (const auto *C = probeAny<llvm::LazyCallGraph::SCC>(IR)) {
    if (C->begin() != C->end())
      return C->begin()->getFunction().getParent();
  }
  return nullptr;
}

bool unitMayMatch(llvm::Any &IR, const ReportOptions &Opts) {
  if (Opts.functionMatcher.empty())
    return true;

  if (const llvm::Module *M = probeAny<llvm::Module>(IR)) {
    for (const llvm::Function &F : *M) {
      if (F.isDeclaration())
        continue;
      if (Opts.functionMatcher.matches(F.getName()) ||
          Opts.functionMatcher.matches(displayName(F.getName(), Opts.demangle)))
        return true;
    }
    return false;
  }
  if (const llvm::Function *F = probeAny<llvm::Function>(IR))
    return Opts.functionMatcher.matches(F->getName()) ||
           Opts.functionMatcher.matches(
               displayName(F->getName(), Opts.demangle));
  if (const llvm::Loop *L = probeAny<llvm::Loop>(IR)) {
    const llvm::Function *F = L->getHeader()->getParent();
    return Opts.functionMatcher.matches(F->getName()) ||
           Opts.functionMatcher.matches(
               displayName(F->getName(), Opts.demangle));
  }
  if (const llvm::LazyCallGraph::SCC *C =
          probeAny<llvm::LazyCallGraph::SCC>(IR)) {
    for (const llvm::LazyCallGraph::Node &N : *C) {
      const llvm::Function &F = N.getFunction();
      if (Opts.functionMatcher.matches(F.getName()) ||
          Opts.functionMatcher.matches(displayName(F.getName(), Opts.demangle)))
        return true;
    }
    return false;
  }
  return true;
}

Snapshot makeSnapshot(llvm::Any &IR, bool CaptureIR) {
  Snapshot S;

  if (const llvm::Module *M = probeAny<llvm::Module>(IR)) {
    S.valid = true;
    S.kind = "module";
    S.name =
        M->getSourceFileName().empty() ? "<module>" : M->getSourceFileName();
    S.display = S.name;
    S.metrics = computeModuleMetrics(*M, &S.functions);
    for (const auto &KV : S.functions)
      S.affected.push_back(KV.first);
    if (CaptureIR)
      S.ir = printModule(*M);
    return S;
  }

  if (const llvm::Function *F = probeAny<llvm::Function>(IR)) {
    S.valid = true;
    S.kind = "function";
    S.name = F->getName().str();
    S.display = displayName(S.name, getOptions().demangle);
    S.metrics = computeFunctionMetrics(*F);
    S.functions[S.name] = S.metrics;
    S.affected.push_back(S.name);
    if (CaptureIR)
      S.ir = printFunction(*F);
    return S;
  }

  if (const llvm::Loop *L = probeAny<llvm::Loop>(IR)) {
    S.valid = true;
    S.kind = "loop";
    const llvm::Function &F = *L->getHeader()->getParent();
    S.name = F.getName().str() + ":loop@" + L->getHeader()->getName().str();
    S.display = displayName(F.getName(), getOptions().demangle) + " loop";
    S.metrics = computeLoopMetrics(*L);
    // Attribute the change to the whole function so function-level timelines
    // and start/end instruction counts stay consistent across loop events.
    S.functions[F.getName().str()] = computeFunctionMetrics(F);
    S.affected.push_back(F.getName().str());
    if (CaptureIR)
      S.ir = printLoop(*L);
    return S;
  }

  if (const llvm::LazyCallGraph::SCC *C =
          probeAny<llvm::LazyCallGraph::SCC>(IR)) {
    S.valid = true;
    S.kind = "scc";
    std::string Name;
    for (const llvm::LazyCallGraph::Node &N : *C) {
      const llvm::Function &F = N.getFunction();
      Metrics FM = computeFunctionMetrics(F);
      S.functions[F.getName().str()] = FM;
      S.affected.push_back(F.getName().str());
      S.metrics.insts += FM.insts;
      S.metrics.bbs += FM.bbs;
      S.metrics.functions += 1;
      S.metrics.loads += FM.loads;
      S.metrics.stores += FM.stores;
      S.metrics.calls += FM.calls;
      S.metrics.branches += FM.branches;
      S.metrics.phis += FM.phis;
      S.metrics.allocas += FM.allocas;
      S.metrics.loops += FM.loops;
      S.metrics.maxLoopDepth =
          std::max(S.metrics.maxLoopDepth, FM.maxLoopDepth);
      S.metrics.callEdges += FM.callEdges;
      if (!Name.empty())
        Name += ",";
      Name += F.getName().str();
    }
    S.name = Name.empty() ? "<scc>" : Name;
    S.display = S.name;
    if (CaptureIR)
      S.ir = printSCC(*C);
    return S;
  }

  S.valid = IR.has_value();
  S.kind = "other";
  S.name = "<?>";
  S.display = S.name;
  return S;
}

class Collector {
public:
  void registerCallbacks(llvm::PassBuilder &PB);

  void beforePass(llvm::StringRef PassID, llvm::Any IR);
  void beforeSkippedPass(llvm::StringRef PassID, llvm::Any IR);
  void afterPass(llvm::StringRef PassID, llvm::Any IR,
                 const llvm::PreservedAnalyses &PA);
  void afterPassInvalidated(llvm::StringRef PassID,
                            const llvm::PreservedAnalyses &PA);
  void emit();

private:
  void captureHeader(const llvm::Module *M);
  void captureDefaultHeader();
  void finalizeEvent(Event E);
  void propagateChild(uint64_t ElapsedNs);
  void recordSkipped(llvm::StringRef PassID, llvm::Any IR);

  std::mutex Mutex;
  std::set<llvm::PassInstrumentationCallbacks *> Registered;
  std::atomic<uint64_t> NextIndex{0};
  std::atomic<uint64_t> OverheadUs{0};
  std::atomic<bool> Emitted{false};

  TraceHeader Header;
  bool HeaderCaptured = false;

  std::unique_ptr<Aggregator> Agg;
  JsonTraceWriter Json;
  bool JsonOpenAttempted = false;
  bool JsonFailed = false;
};

Collector &collector() {
  static Collector C;
  return C;
}

void Collector::captureHeader(const llvm::Module *M) {
  if (HeaderCaptured)
    return;
  HeaderCaptured = true;
  Header.schemaVersion = LPTA_TRACE_SCHEMA_VERSION;
  Header.llvmVersion = LLVM_VERSION_STRING;
  Header.lptaVersion = LPTA_VERSION_STRING;
  Header.route = detectRoute();
  Header.pipeline = getOptions().pipeline;

  if (M) {
    Header.source = M->getSourceFileName();
    Header.targetTriple = M->getTargetTriple().str();
    if (llvm::NamedMDNode *Ident = M->getNamedMetadata("llvm.ident")) {
      if (Ident->getNumOperands() != 0) {
        if (llvm::MDNode *N = Ident->getOperand(0)) {
          if (N->getNumOperands() != 0) {
            if (auto *S = llvm::dyn_cast<llvm::MDString>(N->getOperand(0)))
              Header.producer = S->getString().str();
          }
        }
      }
    }
  }

  if (Header.source.empty()) {
    for (const std::string &A : getProcessArgs()) {
      if (A.empty() || A[0] == '-')
        continue;
      llvm::StringRef Ext = llvm::sys::path::extension(A);
      if (Ext == ".c" || Ext == ".cc" || Ext == ".cpp" || Ext == ".cxx" ||
          Ext == ".rs" || Ext == ".f90" || Ext == ".f95" || Ext == ".f03" ||
          Ext == ".ll" || Ext == ".bc" || Ext == ".swift" || Ext == ".zig" ||
          Ext == ".jl" || Ext == ".d") {
        Header.source = A;
        break;
      }
    }
  }
}

void Collector::captureDefaultHeader() {
  if (HeaderCaptured)
    return;
  HeaderCaptured = true;
  Header.schemaVersion = LPTA_TRACE_SCHEMA_VERSION;
  Header.llvmVersion = LLVM_VERSION_STRING;
  Header.lptaVersion = LPTA_VERSION_STRING;
  Header.route = detectRoute();
  Header.pipeline = getOptions().pipeline;
}

void Collector::registerCallbacks(llvm::PassBuilder &PB) {
  llvm::PassInstrumentationCallbacks *PIC =
      PB.getPassInstrumentationCallbacks();
  if (!PIC) {
    llvm::errs() << "LPTA error: PassInstrumentationCallbacks are unavailable "
                    "in this driver; no events will be recorded.\n";
    return;
  }

  std::lock_guard<std::mutex> Lock(Mutex);
  if (!Registered.insert(PIC).second)
    return;

  static std::once_flag AtExitOnce;
  std::call_once(AtExitOnce, [] { std::atexit(&emitReportAtExit); });

  PIC->registerBeforeNonSkippedPassCallback(
      [this](llvm::StringRef P, llvm::Any IR) {
        beforePass(P, std::move(IR));
      });
  PIC->registerBeforeSkippedPassCallback(
      [this](llvm::StringRef P, llvm::Any IR) {
        beforeSkippedPass(P, std::move(IR));
      });
  PIC->registerAfterPassCallback([this](llvm::StringRef P, llvm::Any IR,
                                        const llvm::PreservedAnalyses &PA) {
    afterPass(P, std::move(IR), PA);
  });
  PIC->registerAfterPassInvalidatedCallback(
      [this](llvm::StringRef P, const llvm::PreservedAnalyses &PA) {
        afterPassInvalidated(P, PA);
      });
}

void Collector::beforePass(llvm::StringRef PassID, llvm::Any IR) {
  const ReportOptions &Opts = getOptions();
  bool NeedAnalysis = Opts.reportEnabled() || !Opts.json.empty() ||
                      !Opts.html.empty() || !Opts.compare.empty();

  Frame F;
  F.pass = PassID.str();
  F.infra = isInfrastructure(PassID);
  F.start = Clock::now();
  // Record the real pass-manager/adaptor nesting, not just nesting between
  // leaf transformation passes.  A function pass normally sits inside a
  // function pass-manager adaptor, and loop/SCC passes sit one level deeper.
  F.depth = static_cast<unsigned>(Frames.size());
  if (!Frames.empty())
    F.parent = Frames.back().pass;

  if (NeedAnalysis && !F.infra && Opts.passMatcher.matches(PassID) &&
      unitMayMatch(IR, Opts)) {
    auto T0 = Clock::now();
    F.before = makeSnapshot(IR, Opts.snapshotsEnabled());
    F.recording = F.before.valid;
    OverheadUs.fetch_add(
        std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - T0)
            .count());
    if (F.recording && !F.before.affected.empty() &&
        !Opts.functionMatcher.empty()) {
      bool Any = false;
      for (const std::string &Name : F.before.affected) {
        if (Opts.functionMatcher.matches(Name) ||
            Opts.functionMatcher.matches(displayName(Name, Opts.demangle))) {
          Any = true;
          break;
        }
      }
      if (!Any)
        F.recording = false;
    }
    if (F.recording) {
      std::lock_guard<std::mutex> Lock(Mutex);
      captureHeader(moduleFromAny(IR));
    }
  }

  Frames.push_back(std::move(F));
}

void Collector::beforeSkippedPass(llvm::StringRef PassID, llvm::Any IR) {
  recordSkipped(PassID, std::move(IR));
}

void Collector::recordSkipped(llvm::StringRef PassID, llvm::Any IR) {
  const ReportOptions &Opts = getOptions();
  if (!Opts.skipped || isInfrastructure(PassID) ||
      !Opts.passMatcher.matches(PassID))
    return;

  Snapshot S = makeSnapshot(IR, false);
  if (!S.valid)
    return;

  Event E;
  E.pass = PassID.str();
  E.unitKind = S.kind;
  E.unitName = S.name;
  E.unitDisplayName = S.display;
  E.affectedFunctions = S.affected;
  E.before = S.metrics;
  E.after = S.metrics;
  E.changed = false;
  E.skipped = true;
  E.durationUs = 0;
  for (const auto &KV : S.functions) {
    FunctionRecord R;
    R.before = KV.second;
    R.after = KV.second;
    E.functions[KV.first] = R;
  }
  std::lock_guard<std::mutex> Lock(Mutex);
  captureHeader(moduleFromAny(IR));
  finalizeEvent(std::move(E));
}

void Collector::propagateChild(uint64_t ElapsedNs) {
  if (!Frames.empty())
    Frames.back().childNs += ElapsedNs;
}

void Collector::afterPass(llvm::StringRef PassID, llvm::Any IR,
                          const llvm::PreservedAnalyses &PA) {
  uint64_t SelfUs = 0;
  Frame F;
  bool Found = false;
  uint64_t ElapsedNs = 0;
  {
    // popFrame only touches thread-local state.
    auto Now = Clock::now();
    for (size_t I = Frames.size(); I > 0; --I) {
      if (Frames[I - 1].pass != PassID)
        continue;
      size_t Index = I - 1;
      F = std::move(Frames[Index]);
      Frames.erase(Frames.begin() + Index, Frames.end());
      ElapsedNs =
          std::chrono::duration_cast<std::chrono::nanoseconds>(Now - F.start)
              .count();
      SelfUs = ElapsedNs > F.childNs ? (ElapsedNs - F.childNs) / 1000 : 0;
      Found = true;
      break;
    }
  }
  if (!Found)
    return;

  if (F.infra) {
    propagateChild(ElapsedNs);
    return;
  }

  if (F.recording) {
    const ReportOptions &Opts = getOptions();
    auto T0 = Clock::now();
    Snapshot After = makeSnapshot(IR, Opts.snapshotsEnabled());
    Event E;
    E.pass = F.pass;
    E.parentPass = F.parent;
    E.depth = F.depth;
    E.durationUs = SelfUs;
    E.unitKind = After.valid ? After.kind : F.before.kind;
    E.unitName = After.valid ? After.name : F.before.name;
    E.unitDisplayName = After.valid ? After.display : F.before.display;
    E.before = F.before.metrics;
    E.after = After.valid ? After.metrics : F.before.metrics;
    E.delta = delta(E.before, E.after);
    E.changed = !PA.areAllPreserved();

    std::set<std::string> Affected;
    for (const std::string &S : F.before.affected)
      Affected.insert(S);
    for (const std::string &S : After.affected)
      Affected.insert(S);
    E.affectedFunctions.assign(Affected.begin(), Affected.end());

    std::set<std::string> FunctionNames;
    for (const auto &KV : F.before.functions)
      FunctionNames.insert(KV.first);
    for (const auto &KV : After.functions)
      FunctionNames.insert(KV.first);
    for (const std::string &Name : FunctionNames) {
      FunctionRecord R;
      auto BI = F.before.functions.find(Name);
      auto AI = After.functions.find(Name);
      if (BI != F.before.functions.end())
        R.before = BI->second;
      if (AI != After.functions.end())
        R.after = AI->second;
      E.functions[Name] = R;
    }

    if (E.changed)
      E.score = significance(E.before, E.delta, Opts.scoreWeights);
    else
      E.score = 0.0;

    if (Opts.snapshotsEnabled() && E.changed && !F.before.ir.empty() &&
        !After.ir.empty()) {
      std::string Label =
          E.unitDisplayName.empty() ? E.unitName : E.unitDisplayName;
      E.diff = unifiedDiff(F.before.ir, After.ir, Label);
    }

    OverheadUs.fetch_add(
        std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - T0)
            .count());

    if (E.score >= Opts.minScore && eventMatches(E, Opts)) {
      std::lock_guard<std::mutex> Lock(Mutex);
      captureHeader(nullptr);
      finalizeEvent(std::move(E));
    }
  }

  propagateChild(ElapsedNs);
}

void Collector::afterPassInvalidated(llvm::StringRef PassID,
                                     const llvm::PreservedAnalyses &PA) {
  uint64_t SelfUs = 0;
  Frame F;
  bool Found = false;
  uint64_t ElapsedNs = 0;
  auto Now = Clock::now();
  for (size_t I = Frames.size(); I > 0; --I) {
    if (Frames[I - 1].pass != PassID)
      continue;
    size_t Index = I - 1;
    F = std::move(Frames[Index]);
    Frames.erase(Frames.begin() + Index, Frames.end());
    ElapsedNs =
        std::chrono::duration_cast<std::chrono::nanoseconds>(Now - F.start)
            .count();
    SelfUs = ElapsedNs > F.childNs ? (ElapsedNs - F.childNs) / 1000 : 0;
    Found = true;
    break;
  }
  if (!Found)
    return;

  if (F.infra) {
    propagateChild(ElapsedNs);
    return;
  }

  if (F.recording) {
    Event E;
    E.pass = F.pass;
    E.parentPass = F.parent;
    E.depth = F.depth;
    E.durationUs = SelfUs;
    E.unitKind = F.before.kind;
    E.unitName = F.before.name;
    E.unitDisplayName = F.before.display;
    E.affectedFunctions = F.before.affected;
    E.before = F.before.metrics;
    E.after = F.before.metrics;
    E.changed = !PA.areAllPreserved();
    E.invalidated = true;
    for (const auto &KV : F.before.functions) {
      FunctionRecord R;
      R.before = KV.second;
      R.after = KV.second;
      E.functions[KV.first] = R;
    }
    std::lock_guard<std::mutex> Lock(Mutex);
    captureHeader(nullptr);
    finalizeEvent(std::move(E));
  }
  propagateChild(ElapsedNs);
}

void Collector::finalizeEvent(Event E) {
  E.index = NextIndex.fetch_add(1);
  const ReportOptions &Opts = getOptions();

  if (!Opts.json.empty() && !JsonFailed) {
    if (!JsonOpenAttempted) {
      JsonOpenAttempted = true;
      std::string Err;
      if (!Json.open(Header, Opts.json, Err)) {
        llvm::errs() << "LPTA error: " << Err << "\n";
        JsonFailed = true;
      }
    }
    if (Json.isOpen()) {
      if (!E.diff.empty()) {
        std::string Err;
        E.diffRef = writeSnapshotDiff(Opts.json, E.index, E.diff, Err);
        if (E.diffRef.empty() && !Err.empty())
          llvm::errs() << "LPTA warning: " << Err << "\n";
      }
      std::string Err;
      if (!Json.write(E, Err)) {
        llvm::errs() << "LPTA error: " << Err << "\n";
        JsonFailed = true;
      }
    }
  }

  if (!Agg)
    Agg = std::make_unique<Aggregator>(Opts);
  Agg->add(E);
}

void Collector::emit() {
  if (Emitted.exchange(true))
    return;

  const ReportOptions &Opts = getOptions();
  std::lock_guard<std::mutex> Lock(Mutex);
  captureDefaultHeader();

  std::string IoErrors;
  bool JsonWritten = false;
  if (!Opts.json.empty()) {
    if (!JsonOpenAttempted) {
      JsonOpenAttempted = true;
      std::string Err;
      if (!Json.open(Header, Opts.json, Err)) {
        IoErrors += "LPTA error: " + Err + "\n";
        JsonFailed = true;
      }
    }
    if (Json.isOpen()) {
      if (JsonFailed) {
        Json.abort();
      } else {
        std::string Err;
        if (Json.finish(Err))
          JsonWritten = true;
        else {
          IoErrors += "LPTA error: " + Err + "\n";
          JsonFailed = true;
        }
      }
    }
  }

  if (!Agg)
    Agg = std::make_unique<Aggregator>(Opts);
  Agg->setOverhead(OverheadUs.load());

  std::string CompareText;
  if (!Opts.compare.empty()) {
    Trace Base;
    std::string Err;
    if (!readTrace(Opts.compare, Base, Err)) {
      CompareText = "COMPARISON FAILED: " + Err;
    } else {
      Trace Current;
      Current.header = Header;
      Current.events = Agg->retainedEvents();
      CompareText = compareTraces(Base, Current, Opts.top);
    }
  }

  std::string Written;
  if (!Opts.html.empty()) {
    std::string Html = renderHtml(Header, *Agg, Opts);
    std::string Err;
    if (writeFileAtomic(Opts.html, Html, Err))
      Written += (Written.empty() ? "Wrote: " : ", ") + Opts.html;
    else
      IoErrors += "LPTA error: " + Err + "\n";
  }
  if (JsonWritten)
    Written += (Written.empty() ? "Wrote: " : ", ") + Opts.json;

  if (Opts.reportEnabled()) {
    std::string Report = renderTerminal(Header, *Agg, Opts, CompareText);
    if (!Written.empty())
      Report += Written + "\n";
    std::string Err;
    if (!Opts.out.empty()) {
      if (!writeFileAtomic(Opts.out, Report, Err))
        IoErrors += "LPTA error: " + Err + "\n";
    } else {
      llvm::errs() << Report;
      llvm::errs().flush();
    }
  } else if (!CompareText.empty()) {
    llvm::errs() << CompareText << "\n";
  }

  if (!IoErrors.empty()) {
    llvm::errs() << IoErrors;
    llvm::errs().flush();
  }
}

} // namespace

void registerCollectorCallbacks(llvm::PassBuilder &PB) {
  collector().registerCallbacks(PB);
}

void emitReportAtExit() { collector().emit(); }

} // namespace lpta
