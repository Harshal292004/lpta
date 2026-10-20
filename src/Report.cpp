#include "Report.h"

#include "Version.h"

#include "llvm/Demangle/Demangle.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/Process.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <iomanip>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace lpta {

namespace {

void addDelta(MetricsDelta &A, const MetricsDelta &B) {
  A.insts += B.insts;
  A.bbs += B.bbs;
  A.functions += B.functions;
  A.loads += B.loads;
  A.stores += B.stores;
  A.calls += B.calls;
  A.branches += B.branches;
  A.phis += B.phis;
  A.allocas += B.allocas;
  A.loops += B.loops;
  A.maxLoopDepth += B.maxLoopDepth;
  A.callEdges += B.callEdges;
}

double weightedMagnitude(const MetricsDelta &D, const ScoreWeights &W) {
  const auto A = [](int64_t V) -> double {
    return static_cast<double>(V < 0 ? -V : V);
  };
  return W.insts * A(D.insts) + W.bbs * A(D.bbs) +
         W.functions * A(D.functions) + W.loads * A(D.loads) +
         W.stores * A(D.stores) + W.calls * A(D.calls) +
         W.branches * A(D.branches) + W.phis * A(D.phis) +
         W.allocas * A(D.allocas) + W.loops * A(D.loops) +
         W.maxLoopDepth * A(D.maxLoopDepth) + W.callEdges * A(D.callEdges);
}

std::string fmtDouble(double V, int Precision) {
  std::ostringstream OS;
  OS << std::fixed << std::setprecision(Precision) << V;
  return OS.str();
}

std::string fmtDelta(int64_t V) {
  if (V > 0)
    return "+" + std::to_string(V);
  return std::to_string(V);
}

std::string fmtTime(uint64_t Us) {
  if (Us < 1000)
    return std::to_string(Us) + "us";
  if (Us < 1000000)
    return fmtDouble(static_cast<double>(Us) / 1000.0, 1) + "ms";
  return fmtDouble(static_cast<double>(Us) / 1000000.0, 2) + "s";
}

std::string fmtPercent(uint64_t Start, uint64_t End) {
  if (Start == 0)
    return End == 0 ? "0%" : "+inf";
  double P = (static_cast<double>(End) - static_cast<double>(Start)) * 100.0 /
             static_cast<double>(Start);
  std::string S = fmtDouble(P, 0);
  if (P > 0)
    S = "+" + S;
  return S + "%";
}

std::string padRight(llvm::StringRef S, size_t Width) {
  std::string R = truncate(S, Width);
  if (R.size() < Width)
    R.append(Width - R.size(), ' ');
  return R;
}

std::string padLeft(llvm::StringRef S, size_t Width) {
  std::string T = truncate(S, Width);
  if (T.size() < Width)
    return std::string(Width - T.size(), ' ') + T;
  return T;
}

std::string escapeHtml(llvm::StringRef S) {
  std::string R;
  R.reserve(S.size());
  for (char C : S) {
    switch (C) {
    case '&':
      R += "&amp;";
      break;
    case '<':
      R += "&lt;";
      break;
    case '>':
      R += "&gt;";
      break;
    case '"':
      R += "&quot;";
      break;
    case '\'':
      R += "&#39;";
      break;
    default:
      R += C;
    }
  }
  return R;
}

struct Style {
  bool Color = false;
  llvm::StringRef bold() const { return Color ? "\033[1m" : ""; }
  llvm::StringRef dim() const { return Color ? "\033[2m" : ""; }
  llvm::StringRef cyan() const { return Color ? "\033[36m" : ""; }
  llvm::StringRef green() const { return Color ? "\033[32m" : ""; }
  llvm::StringRef reset() const { return Color ? "\033[0m" : ""; }
};

std::string sourceOrUnknown(const TraceHeader &H) {
  return H.source.empty() ? "<unknown>" : H.source;
}

std::string producerCompact(const TraceHeader &H) {
  if (H.producer.empty())
    return "unknown";
  std::string P = H.producer;
  for (char &C : P)
    if (C == '\n' || C == '\r' || C == '\t')
      C = ' ';
  return truncate(P, 52);
}

std::string functionKey(const Event &E) {
  if (!E.affectedFunctions.empty()) {
    if (E.affectedFunctions.size() == 1)
      return E.affectedFunctions.front();
    return E.unitName;
  }
  return E.unitName;
}

std::string compareKey(const Event &E) {
  return functionKey(E) + "\x1f" + E.pass;
}

std::string compactUnit(const Event &E) {
  std::string Kind = E.unitKind;
  if (Kind == "module")
    Kind = "mod";
  else if (Kind == "function")
    Kind = "fn";
  std::string Name = E.unitDisplayName.empty() ? E.unitName : E.unitDisplayName;
  if (E.unitKind == "module")
    Name = llvm::sys::path::filename(Name).str();
  return Kind + ":" + Name;
}

bool nonzeroDelta(const MetricsDelta &D) {
  return D.insts != 0 || D.bbs != 0 || D.functions != 0 || D.loads != 0 ||
         D.stores != 0 || D.calls != 0 || D.branches != 0 || D.phis != 0 ||
         D.allocas != 0 || D.loops != 0 || D.maxLoopDepth != 0 ||
         D.callEdges != 0;
}

bool sameMetrics(const Metrics &A, const Metrics &B) {
  return A.insts == B.insts && A.bbs == B.bbs && A.functions == B.functions &&
         A.loads == B.loads && A.stores == B.stores && A.calls == B.calls &&
         A.branches == B.branches && A.phis == B.phis &&
         A.allocas == B.allocas && A.loops == B.loops &&
         A.maxLoopDepth == B.maxLoopDepth && A.callEdges == B.callEdges;
}

/// Recompute a pass score when a function filter reduces a module/loop/SCC
/// event to a subset of its functions.  Unfiltered events keep their original
/// unit-level score.
double filteredScore(const Event &E, const ReportOptions &Opts,
                     const std::vector<std::string> &Functions,
                     const MetricsDelta &Effective) {
  if (Opts.functionMatcher.empty() || E.unitKind == "function")
    return E.score;
  if (!E.changed)
    return 0.0;

  uint64_t BeforeInsts = 0;
  for (const std::string &Name : Functions) {
    auto It = E.functions.find(Name);
    if (It != E.functions.end())
      BeforeInsts += It->second.before.insts;
  }
  Metrics Before;
  Before.insts = BeforeInsts;
  return significance(Before, Effective, Opts.scoreWeights);
}

/// Keep timelines readable: a module pass that did not touch this function is
/// not part of the function's story, even though the module event mentions it.
bool timelineRelevant(const Event &E, llvm::StringRef Function,
                      const MetricsDelta &D) {
  if (E.skipped)
    return false;
  if (E.unitKind == "function")
    return E.unitName == Function && (E.changed || nonzeroDelta(D));
  if (E.unitKind == "module")
    return nonzeroDelta(D);
  return nonzeroDelta(D) || (E.changed && eventHasFunction(E, Function));
}

std::vector<const PassStats *> rankedPasses(const AggregateStats &S) {
  std::vector<const PassStats *> Out;
  for (const auto &KV : S.passes)
    Out.push_back(&KV.second);
  std::sort(Out.begin(), Out.end(), [](const PassStats *A, const PassStats *B) {
    if (A->score != B->score)
      return A->score > B->score;
    if (A->delta.insts != B->delta.insts)
      return std::llabs(A->delta.insts) > std::llabs(B->delta.insts);
    return A->name < B->name;
  });
  return Out;
}

std::vector<const FunctionStats *> rankedFunctions(const AggregateStats &S) {
  std::vector<const FunctionStats *> Out;
  for (const auto &KV : S.functions)
    Out.push_back(&KV.second);
  std::sort(Out.begin(), Out.end(),
            [](const FunctionStats *A, const FunctionStats *B) {
              if (A->totalChange != B->totalChange)
                return A->totalChange > B->totalChange;
              if (A->totalAbsInsts != B->totalAbsInsts)
                return A->totalAbsInsts > B->totalAbsInsts;
              return A->name < B->name;
            });
  return Out;
}

std::string changedBy(const FunctionStats &F, size_t Count) {
  std::vector<std::pair<std::string, double>> P(F.passContribution.begin(),
                                                F.passContribution.end());
  std::sort(P.begin(), P.end(), [](const auto &A, const auto &B) {
    if (A.second != B.second)
      return A.second > B.second;
    return A.first < B.first;
  });
  std::string R;
  size_t N = 0;
  for (const auto &KV : P) {
    if (KV.second <= 0)
      continue;
    if (N)
      R += ", ";
    R += prettyPassName(KV.first);
    if (++N == Count)
      break;
  }
  return R.empty() ? "-" : R;
}

} // namespace

NameFilter::NameFilter(llvm::StringRef Pattern) : Text(Pattern.str()) {
  if (Text.empty())
    return;
  auto R = std::make_shared<llvm::Regex>(Text);
  std::string Err;
  if (R->isValid(Err))
    RE = std::move(R);
  else
    llvm::errs() << "LPTA warning: invalid regex '" << Text << "' (" << Err
                 << "); using substring matching\n";
}

bool NameFilter::matches(llvm::StringRef Name) const {
  if (Text.empty())
    return true;
  if (RE)
    return RE->match(Name);
  return Name.contains(Text);
}

bool ReportOptions::retainEvents() const {
  if (view == "functions" || view == "full")
    return true;
  if (!html.empty() || !compare.empty())
    return true;
  if (diff && reportEnabled())
    return true;
  return false;
}

void ReportOptions::compileFilters() {
  functionMatcher = NameFilter(functionFilter);
  passMatcher = NameFilter(passFilter);
}

std::string demangleName(llvm::StringRef Raw) {
  std::string D = llvm::demangle(Raw.str());
  return D.empty() ? Raw.str() : D;
}

std::string displayName(llvm::StringRef Raw, llvm::StringRef DemangleMode) {
  if (DemangleMode == "off")
    return Raw.str();
  return demangleName(Raw);
}

std::string prettyPassName(llvm::StringRef Name) {
  if (Name.ends_with("Pass") && Name.size() > 4)
    return Name.drop_back(4).str();
  return Name.str();
}

std::string truncate(llvm::StringRef S, size_t Width) {
  if (S.size() <= Width)
    return S.str();
  if (Width <= 3)
    return S.substr(0, Width).str();
  return S.substr(0, Width - 3).str() + "...";
}

static std::string clipBlock(llvm::StringRef Text, size_t Width) {
  std::string Result;
  llvm::raw_string_ostream OS(Result);
  size_t Start = 0;
  while (Start < Text.size()) {
    size_t End = Text.find('\n', Start);
    bool HasNewline = End != llvm::StringRef::npos;
    llvm::StringRef Line =
        Text.substr(Start, HasNewline ? End - Start : llvm::StringRef::npos);
    OS << truncate(Line, Width);
    if (HasNewline) {
      OS << '\n';
      Start = End + 1;
    } else {
      break;
    }
  }
  return OS.str();
}

bool eventHasFunction(const Event &E, llvm::StringRef Function) {
  if (E.affectedFunctions.empty())
    return E.unitName == Function || E.unitDisplayName == Function;
  for (const std::string &F : E.affectedFunctions)
    if (F == Function)
      return true;
  return false;
}

MetricsDelta FunctionRecord::effectiveDelta() const {
  return hasDelta ? storedDelta : lpta::delta(before, after);
}

bool eventMatches(const Event &E, const ReportOptions &Opts) {
  if (!Opts.passMatcher.matches(E.pass))
    return false;
  if (E.score < Opts.minScore)
    return false;
  if (Opts.functionMatcher.empty())
    return true;

  if (!E.affectedFunctions.empty()) {
    for (const std::string &F : E.affectedFunctions) {
      if (Opts.functionMatcher.matches(F) ||
          Opts.functionMatcher.matches(displayName(F, Opts.demangle)))
        return true;
    }
  }
  if (Opts.functionMatcher.matches(E.unitName) ||
      Opts.functionMatcher.matches(E.unitDisplayName))
    return true;
  return false;
}

void Aggregator::add(const Event &E) {
  if (!eventMatches(E, Opts))
    return;

  if (E.skipped) {
    if (Opts.retainEvents())
      Events.push_back(E);
    return;
  }

  std::vector<std::string> MatchedFunctions;
  if (Opts.functionMatcher.empty()) {
    for (const auto &KV : E.functions)
      MatchedFunctions.push_back(KV.first);
  } else {
    for (const auto &KV : E.functions) {
      if (Opts.functionMatcher.matches(KV.first) ||
          Opts.functionMatcher.matches(displayName(KV.first, Opts.demangle)))
        MatchedFunctions.push_back(KV.first);
    }
  }
  if (Opts.functionMatcher.empty() && MatchedFunctions.empty() &&
      !E.affectedFunctions.empty())
    MatchedFunctions = E.affectedFunctions;
  if (!Opts.functionMatcher.empty() && MatchedFunctions.empty())
    return;

  MetricsDelta Effective = E.delta;
  if (!Opts.functionMatcher.empty() && E.unitKind != "function") {
    Effective = MetricsDelta();
    for (const std::string &F : MatchedFunctions) {
      auto It = E.functions.find(F);
      if (It != E.functions.end())
        addDelta(Effective, It->second.effectiveDelta());
    }
  }

  PassStats &P = Stats.passes[E.pass];
  P.name = E.pass;
  ++P.runs;
  if (E.changed)
    ++P.changed;
  addDelta(P.delta, Effective);
  P.durationUs += E.durationUs;
  double EventScore = filteredScore(E, Opts, MatchedFunctions, Effective);
  P.score += EventScore;

  ++Stats.passesRun;
  if (E.changed)
    ++Stats.changedPasses;
  Stats.totalTimeUs += E.durationUs;

  for (const std::string &F : MatchedFunctions) {
    FunctionStats &FS = Stats.functions[F];
    FS.name = F;
    if (FS.displayName.empty())
      FS.displayName = displayName(F, Opts.demangle);
    Stats.functionNames.insert(F);

    auto It = E.functions.find(F);
    if (It == E.functions.end())
      continue;
    const FunctionRecord &R = It->second;
    if (!FS.haveInsts) {
      FS.firstInsts = R.before.insts;
      FS.haveInsts = true;
    }
    FS.lastInsts = R.after.insts;
    MetricsDelta D = R.effectiveDelta();
    double Mag = weightedMagnitude(D, Opts.scoreWeights);
    FS.totalAbsInsts += static_cast<uint64_t>(std::llabs(D.insts));
    FS.totalChange += Mag;
    FS.totalScore += EventScore;
    FS.passContribution[E.pass] += Mag;
    if (E.changed)
      ++FS.changedPasses;
  }

  if (Opts.retainEvents())
    Events.push_back(E);
}

bool isColorEnabled(llvm::StringRef Mode, bool WritingToTerminal,
                    bool NoColorEnv) {
  if (Mode == "always")
    return true;
  if (Mode == "never" || NoColorEnv)
    return false;
  return WritingToTerminal;
}

bool writeFileAtomic(llvm::StringRef Path, llvm::StringRef Contents,
                     std::string &Err) {
  llvm::SmallString<256> Parent(Path);
  llvm::sys::path::remove_filename(Parent);
  if (!Parent.empty()) {
    std::error_code EC = llvm::sys::fs::create_directories(Parent);
    if (EC) {
      Err = "cannot create directory '" + Parent.str().str() +
            "': " + EC.message();
      return false;
    }
  }

  std::string Temp =
      (Path + ".tmp." + std::to_string(llvm::sys::Process::getProcessId()))
          .str();
  {
    std::error_code EC;
    llvm::raw_fd_ostream OS(
        Temp, EC, llvm::sys::fs::CreationDisposition::CD_CreateAlways);
    if (EC) {
      Err = "cannot open '" + Temp + "': " + EC.message();
      return false;
    }
    OS << Contents;
    OS.close();
    if (OS.has_error()) {
      Err = "error writing '" + Temp + "'";
      llvm::sys::fs::remove(Temp);
      return false;
    }
  }
  std::error_code EC = llvm::sys::fs::rename(Temp, Path);
  if (EC) {
    Err =
        "cannot rename '" + Temp + "' to '" + Path.str() + "': " + EC.message();
    llvm::sys::fs::remove(Temp);
    return false;
  }
  return true;
}

std::string renderTerminal(const TraceHeader &Header, const Aggregator &Agg,
                           const ReportOptions &Opts,
                           llvm::StringRef CompareText) {
  const AggregateStats &S = Agg.stats();
  bool TTY = false;
#if !defined(_WIN32)
  TTY = llvm::sys::Process::StandardErrHasColors();
#endif
  bool NoColor = llvm::sys::Process::GetEnv("NO_COLOR").has_value();
  Style St;
  St.Color = isColorEnabled(Opts.color, TTY && Opts.out.empty(), NoColor);

  std::string Result;
  llvm::raw_string_ostream OS(Result);

  OS << St.bold() << "LPTA" << St.reset() << "  "
     << truncate(sourceOrUnknown(Header), 72) << "\n";
  OS << "producer=" << truncate(producerCompact(Header), 52) << "  LLVM "
     << truncate(Header.llvmVersion.empty() ? "?" : Header.llvmVersion, 10)
     << "\n";
  OS << "route="
     << truncate(Header.route.empty() ? "unknown" : Header.route, 18)
     << "  pipeline="
     << truncate(Header.pipeline.empty() ? "unknown" : Header.pipeline, 44)
     << "\n";
  std::string StatsLine = "passes " + std::to_string(S.passesRun) +
                          " | changed " + std::to_string(S.changedPasses) +
                          " | functions " +
                          std::to_string(S.functionNames.size()) +
                          " | overhead " + fmtTime(S.overheadUs);
  OS << truncate(StatsLine, 80) << "\n";

  std::vector<const PassStats *> Passes = rankedPasses(S);
  std::vector<const FunctionStats *> Functions = rankedFunctions(S);
  unsigned Top = Opts.top;

  auto renderTopPasses = [&] {
    OS << "\n"
       << St.bold() << "TOP PASSES BY SIGNIFICANCE" << St.reset() << "\n";
    OS << " #  " << padRight("pass", 24) << padLeft("runs", 6)
       << padLeft("changed", 9) << padLeft("d_insts", 9)
       << padLeft("d_calls", 9) << padLeft("ms", 8) << padLeft("score", 7)
       << "\n";
    size_t N = 0;
    for (const PassStats *P : Passes) {
      if (Top != 0 && N >= Top)
        break;
      OS << " " << padLeft(std::to_string(N + 1), 2) << "  "
         << padRight(truncate(P->name, 24), 24)
         << padLeft(std::to_string(P->runs), 6)
         << padLeft(std::to_string(P->changed), 9)
         << padLeft(fmtDelta(P->delta.insts), 9)
         << padLeft(fmtDelta(P->delta.calls), 9)
         << padLeft(fmtDouble(static_cast<double>(P->durationUs) / 1000.0, 1),
                    8)
         << padLeft(fmtDouble(P->score, 2), 7) << "\n";
      ++N;
    }
    if (N == 0)
      OS << "  (no matching passes)\n";
  };

  auto renderTopFunctions = [&] {
    OS << "\n"
       << St.bold() << "TOP FUNCTIONS BY TOTAL CHANGE" << St.reset() << "\n";
    OS << " #  " << padRight("function", 30)
       << padRight("insts (start -> end)", 24) << "changed by\n";
    size_t N = 0;
    for (const FunctionStats *F : Functions) {
      if (!F->haveInsts)
        continue;
      if (Top != 0 && N >= Top)
        break;
      std::string Range = std::to_string(F->firstInsts) + " -> " +
                          std::to_string(F->lastInsts) + " (" +
                          fmtPercent(F->firstInsts, F->lastInsts) + ")";
      OS << " " << padLeft(std::to_string(N + 1), 2) << "  "
         << padRight(F->displayName, 30) << padRight(Range, 24)
         << truncate(changedBy(*F, 3), 21) << "\n";
      ++N;
    }
    if (N == 0)
      OS << "  (no matching functions)\n";
  };

  auto renderPassesView = [&] {
    OS << "\n" << St.bold() << "PASSES BY SIGNIFICANCE" << St.reset() << "\n";
    OS << " #  " << padRight("pass", 24) << padLeft("runs", 6)
       << padLeft("changed", 9) << padLeft("d_insts", 9)
       << padLeft("d_calls", 9) << padLeft("ms", 8) << padLeft("score", 7)
       << "\n";
    size_t N = 0;
    for (const PassStats *P : Passes) {
      if (Top != 0 && N >= Top)
        break;
      OS << " " << padLeft(std::to_string(N + 1), 2) << "  "
         << padRight(truncate(P->name, 24), 24)
         << padLeft(std::to_string(P->runs), 6)
         << padLeft(std::to_string(P->changed), 9)
         << padLeft(fmtDelta(P->delta.insts), 9)
         << padLeft(fmtDelta(P->delta.calls), 9)
         << padLeft(fmtDouble(static_cast<double>(P->durationUs) / 1000.0, 1),
                    8)
         << padLeft(fmtDouble(P->score, 2), 7) << "\n";
      ++N;
    }
    if (N == 0)
      OS << "  (no matching passes)\n";
  };

  auto renderFunctionsView = [&] {
    OS << "\n" << St.bold() << "FUNCTION TIMELINES" << St.reset() << "\n";
    size_t N = 0;
    for (const FunctionStats *F : Functions) {
      if (Top != 0 && N >= Top)
        break;
      OS << "\n" << St.cyan();
      std::string Title = F->displayName;
      if (F->displayName != F->name)
        Title += "  [" + F->name + "]";
      if (F->haveInsts)
        Title += "  insts " + std::to_string(F->firstInsts) + " -> " +
                 std::to_string(F->lastInsts) + " (" +
                 fmtPercent(F->firstInsts, F->lastInsts) + ")";
      OS << truncate(Title, 80) << St.reset() << "\n";
      OS << " #    " << padRight("pass", 24) << padLeft("unit", 10)
         << padLeft("d_insts", 9) << padLeft("d_calls", 9) << padLeft("ms", 8)
         << padLeft("score", 7) << "\n";

      size_t Rows = 0;
      for (const Event &E : Agg.retainedEvents()) {
        auto It = E.functions.find(F->name);
        if (It == E.functions.end())
          continue;
        MetricsDelta D = It->second.effectiveDelta();
        if (!timelineRelevant(E, F->name, D))
          continue;
        OS << " " << padLeft(std::to_string(E.index), 4) << "  "
           << padRight(truncate(E.pass, 24), 24) << padLeft(E.unitKind, 10)
           << padLeft(fmtDelta(D.insts), 9) << padLeft(fmtDelta(D.calls), 9)
           << padLeft(fmtDouble(static_cast<double>(E.durationUs) / 1000.0, 1),
                      8)
           << padLeft(fmtDouble(E.score, 2), 7) << "\n";
        ++Rows;
      }
      if (Rows == 0)
        OS << "  (no retained invocations; use -lpta-report=functions)\n";
      ++N;
    }
    if (N == 0)
      OS << "  (no matching functions)\n";
  };

  auto renderFullView = [&] {
    OS << "\n" << St.bold() << "ALL INVOCATIONS" << St.reset() << "\n";
    OS << " #    " << padRight("pass", 22) << " " << padRight("unit", 14)
       << padLeft("chg", 4) << padLeft("d_insts", 8) << padLeft("d_calls", 8)
       << padLeft("ms", 7) << padLeft("score", 7) << "\n";
    size_t N = 0;
    for (const Event &E : Agg.retainedEvents()) {
      if (!eventMatches(E, Opts))
        continue;
      std::string Unit = compactUnit(E);
      OS << " " << padLeft(std::to_string(E.index), 4) << "  "
         << padRight(truncate(E.pass, 22), 22) << " "
         << padRight(truncate(Unit, 14), 14)
         << padLeft(E.skipped ? "skip" : (E.changed ? "yes" : "-"), 4)
         << padLeft(fmtDelta(E.delta.insts), 8)
         << padLeft(fmtDelta(E.delta.calls), 8)
         << padLeft(fmtDouble(static_cast<double>(E.durationUs) / 1000.0, 1), 7)
         << padLeft(fmtDouble(E.score, 2), 7) << "\n";
      ++N;
    }
    if (N == 0)
      OS << "  (no matching invocations)\n";
  };

  if (Opts.view == "summary") {
    renderTopPasses();
    renderTopFunctions();
  } else if (Opts.view == "passes") {
    renderPassesView();
  } else if (Opts.view == "functions") {
    renderFunctionsView();
  } else if (Opts.view == "full") {
    renderFullView();
  } else if (Opts.view != "summary") {
    OS << "\n(unknown view '" << Opts.view << "'; expected summary, passes, "
       << "functions, full, or off)\n";
  }

  if (!CompareText.empty())
    OS << "\n" << CompareText;

  if (Opts.diff && Opts.view != "off") {
    bool Header = false;
    for (const Event &E : Agg.retainedEvents()) {
      if (!E.changed || E.diff.empty() || !eventMatches(E, Opts))
        continue;
      if (!Header) {
        OS << "\n" << St.bold() << "IR DIFFS" << St.reset() << "\n";
        Header = true;
      }
      OS << "\n" << St.dim();
      std::string DiffHeader =
          "#" + std::to_string(E.index) + " " + E.pass + " on " +
          (E.unitDisplayName.empty() ? E.unitName : E.unitDisplayName);
      OS << truncate(DiffHeader, 80) << St.reset() << "\n" << E.diff;
    }
  }

  OS << "\n"
     << St.dim()
     << "score is a heuristic for ranking change magnitude, not a predicted "
        "speedup."
     << St.reset() << "\n";
  OS << "More: -lpta-report=functions | -lpta-function=<name> | "
        "-lpta-html=report.html\n";
  return OS.str();
}

std::string renderHtml(const TraceHeader &Header, const Aggregator &Agg,
                       const ReportOptions &Opts) {
  const AggregateStats &S = Agg.stats();
  std::vector<const PassStats *> Passes = rankedPasses(S);
  std::vector<const FunctionStats *> Functions = rankedFunctions(S);

  std::string R;
  llvm::raw_string_ostream OS(R);
  OS << "<!doctype html>\n<html lang=\"en\"><head><meta charset=\"utf-8\">\n"
     << "<meta name=\"viewport\" content=\"width=device-width, "
        "initial-scale=1\">\n"
     << "<title>LPTA - " << escapeHtml(sourceOrUnknown(Header)) << "</title>\n"
     << "<style>\n"
     << "body{font:14px/1.45 "
        "ui-monospace,SFMono-Regular,Menlo,Consolas,monospace;"
        "margin:2rem;max-width:1200px;color:#1b1f23;background:#fff}\n"
     << "h1{font-size:1.4rem;margin:0 0 .3rem}h2{font-size:1.05rem;margin:2rem "
        "0 .6rem;"
        "border-bottom:1px solid #d8dee4;padding-bottom:.25rem}\n"
     << ".meta{color:#57606a}.stats{margin:.5rem 0 1rem;padding:.6rem .8rem;"
        "background:#f6f8fa;border:1px solid #d8dee4;border-radius:6px}\n"
     << "table{border-collapse:collapse;width:100%;margin:.4rem 0 1rem}"
        "th,td{text-align:left;padding:.3rem .5rem;border-bottom:1px solid "
        "#eaeef2;"
        "vertical-align:top}th{background:#f6f8fa;font-weight:600}"
        "td.num,th.num{text-align:right}code{background:#f6f8fa;padding:.1rem "
        ".25rem;"
        "border-radius:4px}details{margin:.5rem 0;border:1px solid #d8dee4;"
        "border-radius:6px;padding:.4rem .7rem}summary{cursor:pointer}"
        "pre{overflow:auto;background:#f6f8fa;padding:.7rem;border-radius:6px;"
        "font-size:12px}hr{border:0;border-top:1px solid #d8dee4;margin:1.5rem "
        "0}"
        ".note{color:#57606a;font-size:12px}</style></head><body>\n";

  OS << "<h1>LPTA <span class=\"meta\">" << escapeHtml(sourceOrUnknown(Header))
     << "</span></h1>\n<div class=\"meta\">producer="
     << escapeHtml(producerCompact(Header))
     << " &middot; route=" << escapeHtml(Header.route)
     << " &middot; pipeline=" << escapeHtml(Header.pipeline)
     << " &middot; LLVM " << escapeHtml(Header.llvmVersion) << "</div>\n";
  OS << "<div class=\"stats\">passes run <b>" << S.passesRun
     << "</b> &middot; changed <b>" << S.changedPasses
     << "</b> &middot; functions <b>" << S.functionNames.size()
     << "</b> &middot; overhead <b>" << fmtTime(S.overheadUs) << "</b></div>\n";

  OS << "<h2>Top passes by "
        "significance</h2>\n<table><tr><th>#</th><th>pass</th>"
        "<th class=\"num\">runs</th><th class=\"num\">changed</th>"
        "<th class=\"num\">d_insts</th><th class=\"num\">d_calls</th>"
        "<th class=\"num\">ms</th><th class=\"num\">score</th></tr>\n";
  size_t N = 0;
  for (const PassStats *P : Passes) {
    if (Opts.top != 0 && N >= Opts.top)
      break;
    OS << "<tr><td>" << (++N) << "</td><td><code>" << escapeHtml(P->name)
       << "</code></td><td class=\"num\">" << P->runs
       << "</td><td class=\"num\">" << P->changed << "</td><td class=\"num\">"
       << fmtDelta(P->delta.insts) << "</td><td class=\"num\">"
       << fmtDelta(P->delta.calls) << "</td><td class=\"num\">"
       << fmtDouble(static_cast<double>(P->durationUs) / 1000.0, 1)
       << "</td><td class=\"num\">" << fmtDouble(P->score, 2) << "</td></tr>\n";
  }
  OS << "</table>\n";

  OS << "<h2>Top functions by total change</h2>\n<table><tr><th>#</th>"
        "<th>function</th><th>insts (start -&gt; end)</th><th>changed by</th>"
        "</tr>\n";
  N = 0;
  for (const FunctionStats *F : Functions) {
    if (!F->haveInsts)
      continue;
    if (Opts.top != 0 && N >= Opts.top)
      break;
    OS << "<tr><td>" << (++N) << "</td><td><code>" << escapeHtml(F->displayName)
       << "</code></td><td>" << F->firstInsts << " -&gt; " << F->lastInsts
       << " (" << fmtPercent(F->firstInsts, F->lastInsts) << ")</td><td>"
       << escapeHtml(changedBy(*F, 3)) << "</td></tr>\n";
  }
  OS << "</table>\n";

  OS << "<h2>Function timelines</h2>\n";
  N = 0;
  for (const FunctionStats *F : Functions) {
    if (Opts.top != 0 && N >= Opts.top)
      break;
    OS << "<details><summary><b>" << escapeHtml(F->displayName) << "</b>";
    if (F->displayName != F->name)
      OS << " <span class=\"meta\">[" << escapeHtml(F->name) << "]</span>";
    if (F->haveInsts)
      OS << " &middot; insts " << F->firstInsts << " -&gt; " << F->lastInsts
         << " (" << fmtPercent(F->firstInsts, F->lastInsts) << ")";
    OS << "</summary>\n<table><tr><th>#</th><th>pass</th><th>unit</th>"
          "<th class=\"num\">d_insts</th><th class=\"num\">d_calls</th>"
          "<th class=\"num\">ms</th><th class=\"num\">score</th></tr>\n";
    for (const Event &E : Agg.retainedEvents()) {
      auto It = E.functions.find(F->name);
      if (It == E.functions.end())
        continue;
      MetricsDelta D = It->second.effectiveDelta();
      if (!timelineRelevant(E, F->name, D))
        continue;
      OS << "<tr><td>" << E.index << "</td><td><code>" << escapeHtml(E.pass)
         << "</code></td><td>" << escapeHtml(E.unitKind)
         << "</td><td class=\"num\">" << fmtDelta(D.insts)
         << "</td><td class=\"num\">" << fmtDelta(D.calls)
         << "</td><td class=\"num\">"
         << fmtDouble(static_cast<double>(E.durationUs) / 1000.0, 1)
         << "</td><td class=\"num\">" << fmtDouble(E.score, 2)
         << "</td></tr>\n";
    }
    OS << "</table></details>\n";
    ++N;
  }

  OS << "<h2>IR diffs</h2>\n";
  bool AnyDiff = false;
  for (const Event &E : Agg.retainedEvents()) {
    if (!E.changed || E.diff.empty() || !eventMatches(E, Opts))
      continue;
    AnyDiff = true;
    OS << "<details><summary>#" << E.index << " <code>" << escapeHtml(E.pass)
       << "</code> on "
       << escapeHtml(E.unitDisplayName.empty() ? E.unitName : E.unitDisplayName)
       << "</summary><pre>" << escapeHtml(E.diff) << "</pre></details>\n";
  }
  if (!AnyDiff)
    OS << "<p class=\"note\">No diffs captured. Use <code>-lpta-html</code> "
          "with a pipeline that changes IR.</p>\n";

  OS << "<hr><p class=\"note\">Schema " << LPTA_TRACE_SCHEMA_VERSION
     << " &middot; LPTA " << LPTA_VERSION_STRING
     << " &middot; score is a heuristic for ranking change magnitude, not a "
        "predicted speedup.</p>\n</body></html>\n";
  return OS.str();
}

std::string compareTraces(const Trace &Base, const Trace &Current,
                          unsigned Top) {
  std::map<std::string, std::vector<const Event *>> BaseByKey;
  for (const Event &E : Base.events)
    BaseByKey[compareKey(E)].push_back(&E);
  std::map<std::string, size_t> Seen;

  const Event *FirstBase = nullptr;
  const Event *FirstCur = nullptr;
  std::string Reason;

  for (const Event &Cur : Current.events) {
    std::string Key = compareKey(Cur);
    size_t Occ = Seen[Key]++;
    auto It = BaseByKey.find(Key);
    if (It == BaseByKey.end() || Occ >= It->second.size()) {
      FirstCur = &Cur;
      Reason = "invocation is missing from the baseline trace";
      break;
    }
    const Event *BaseE = It->second[Occ];
    if (!sameMetrics(BaseE->before, Cur.before) ||
        !sameMetrics(BaseE->after, Cur.after) ||
        BaseE->changed != Cur.changed || BaseE->unitKind != Cur.unitKind) {
      FirstBase = BaseE;
      FirstCur = &Cur;
      Reason = "IR metrics differ";
      break;
    }
  }

  if (!FirstCur) {
    for (const Event &BaseE : Base.events) {
      std::string Key = compareKey(BaseE);
      auto It = Seen.find(Key);
      size_t SeenCount = It == Seen.end() ? 0 : It->second;
      auto BIt = BaseByKey.find(Key);
      if (BIt == BaseByKey.end())
        continue;
      size_t BaseOcc = 0;
      for (const Event *E : BIt->second) {
        if (E == &BaseE)
          break;
        ++BaseOcc;
      }
      if (BaseOcc >= SeenCount) {
        FirstBase = &BaseE;
        Reason = "invocation is missing from the current trace";
        break;
      }
    }
  }

  std::string R;
  llvm::raw_string_ostream OS(R);
  OS << "CROSS-RUN COMPARISON\n";
  OS << "baseline: " << sourceOrUnknown(Base.header)
     << "  pipeline=" << Base.header.pipeline << "  LLVM "
     << Base.header.llvmVersion << "\n";
  OS << "current:  " << sourceOrUnknown(Current.header)
     << "  pipeline=" << Current.header.pipeline << "  LLVM "
     << Current.header.llvmVersion << "\n";

  if (FirstCur) {
    OS << "\nFIRST DIVERGENCE: " << Reason << "\n";
    OS << "  pass=" << FirstCur->pass << "  unit=" << FirstCur->unitKind << ":"
       << (FirstCur->unitDisplayName.empty() ? FirstCur->unitName
                                             : FirstCur->unitDisplayName)
       << "  index=" << FirstCur->index << "\n";
    OS << "  baseline: ";
    if (FirstBase)
      OS << FirstBase->before.insts << " -> " << FirstBase->after.insts
         << " insts, changed=" << (FirstBase->changed ? "yes" : "no");
    else
      OS << "(absent)";
    OS << "\n  current:  " << FirstCur->before.insts << " -> "
       << FirstCur->after.insts
       << " insts, changed=" << (FirstCur->changed ? "yes" : "no") << "\n";
  } else {
    OS << "\nNo metric divergence found (timing and score are ignored).\n";
  }

  ReportOptions Opts;
  Opts.view = "passes";
  Opts.top = 0;
  Aggregator BA(Opts), CA(Opts);
  for (const Event &E : Base.events)
    BA.add(E);
  for (const Event &E : Current.events)
    CA.add(E);

  OS << "\nPER-PASS METRIC DIFFERENCES\n";
  OS << " " << padRight("pass", 28) << padLeft("runs b/c", 11)
     << padLeft("d_insts b/c", 15) << padLeft("d_calls b/c", 15) << "\n";
  std::set<std::string> Names;
  for (const auto &KV : BA.stats().passes)
    Names.insert(KV.first);
  for (const auto &KV : CA.stats().passes)
    Names.insert(KV.first);
  unsigned Shown = 0;
  for (const std::string &Name : Names) {
    const PassStats *B = nullptr;
    const PassStats *C = nullptr;
    auto BI = BA.stats().passes.find(Name);
    auto CI = CA.stats().passes.find(Name);
    if (BI != BA.stats().passes.end())
      B = &BI->second;
    if (CI != CA.stats().passes.end())
      C = &CI->second;
    uint64_t BR = B ? B->runs : 0, CR = C ? C->runs : 0;
    int64_t BIv = B ? B->delta.insts : 0, CIv = C ? C->delta.insts : 0;
    int64_t BCa = B ? B->delta.calls : 0, CCa = C ? C->delta.calls : 0;
    if (BR == CR && BIv == CIv && BCa == CCa)
      continue;
    OS << " " << padRight(truncate(Name, 28), 28)
       << padLeft(std::to_string(BR) + "/" + std::to_string(CR), 11)
       << padLeft(fmtDelta(BIv) + "/" + fmtDelta(CIv), 15)
       << padLeft(fmtDelta(BCa) + "/" + fmtDelta(CCa), 15) << "\n";
    if (Top != 0 && ++Shown >= Top)
      break;
  }
  if (Shown == 0 && FirstCur)
    OS << " (no aggregate difference; divergence is below pass level)\n";
  return clipBlock(OS.str(), 80);
}

} // namespace lpta
