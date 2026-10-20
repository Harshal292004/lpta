#include "Options.h"

#include "Metrics.h"

#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/Process.h"
#include "llvm/Support/raw_ostream.h"

#include <cstdlib>
#include <fstream>
#include <mutex>
#include <string>
#include <vector>

#if defined(__APPLE__)
#include <crt_externs.h>
#endif

namespace lpta {

namespace {

llvm::cl::opt<std::string> ClReport(
    "lpta-report",
    llvm::cl::desc("Terminal view: summary, passes, functions, full, off"),
    llvm::cl::init("summary"));
llvm::cl::opt<unsigned> ClTop("lpta-top",
                              llvm::cl::desc("Rows in ranked tables (0 = all)"),
                              llvm::cl::init(10));
llvm::cl::opt<std::string> ClFunction(
    "lpta-function",
    llvm::cl::desc("Limit views to functions matching a name or regex"),
    llvm::cl::init(""));
llvm::cl::opt<std::string>
    ClPass("lpta-pass",
           llvm::cl::desc("Limit views to passes matching a name or regex"),
           llvm::cl::init(""));
llvm::cl::opt<double>
    ClMinScore("lpta-min-score",
               llvm::cl::desc("Hide invocations below this score"),
               llvm::cl::init(0.0));
llvm::cl::opt<bool>
    ClDiff("lpta-diff",
           llvm::cl::desc("Capture and print IR diffs for changed passes"),
           llvm::cl::init(false));
llvm::cl::opt<std::string> ClColor("lpta-color",
                                   llvm::cl::desc("Color: auto, always, never"),
                                   llvm::cl::init("auto"));
llvm::cl::opt<std::string>
    ClOut("lpta-out", llvm::cl::desc("Write the terminal report to this file"),
          llvm::cl::init(""));
llvm::cl::opt<std::string>
    ClHtml("lpta-html", llvm::cl::desc("Write a self-contained HTML report"),
           llvm::cl::init(""));
llvm::cl::opt<std::string>
    ClJson("lpta-json", llvm::cl::desc("Write a versioned JSON trace"),
           llvm::cl::init(""));
llvm::cl::opt<std::string>
    ClCompare("lpta-compare",
              llvm::cl::desc("Compare this run against a saved trace"),
              llvm::cl::init(""));
llvm::cl::opt<std::string>
    ClWeights("lpta-weights", llvm::cl::desc("Significance weights file"),
              llvm::cl::init(""));
llvm::cl::opt<std::string>
    ClDemangle("lpta-demangle", llvm::cl::desc("Demangle names: auto, off"),
               llvm::cl::init("auto"));
llvm::cl::opt<std::string>
    ClPipeline("lpta-pipeline",
               llvm::cl::desc("Override the pipeline label in reports"),
               llvm::cl::init(""));
llvm::cl::opt<bool> ClSkipped("lpta-skipped",
                              llvm::cl::desc("Record skipped passes as events"),
                              llvm::cl::init(false));

std::vector<std::string> splitShellWords(llvm::StringRef Text) {
  std::vector<std::string> Words;
  std::string Cur;
  bool InSingle = false;
  bool InDouble = false;
  bool Escape = false;

  for (char C : Text) {
    if (Escape) {
      Cur += C;
      Escape = false;
      continue;
    }
    if (C == '\\' && !InSingle) {
      Escape = true;
      continue;
    }
    if (C == '\'' && !InDouble) {
      InSingle = !InSingle;
      continue;
    }
    if (C == '"' && !InSingle) {
      InDouble = !InDouble;
      continue;
    }
    if (!InSingle && !InDouble && (C == ' ' || C == '\t' || C == '\n')) {
      if (!Cur.empty()) {
        Words.push_back(Cur);
        Cur.clear();
      }
      continue;
    }
    Cur += C;
  }
  if (!Cur.empty())
    Words.push_back(Cur);
  return Words;
}

bool parseBool(llvm::StringRef S, bool &Out) {
  if (S == "1" || S == "true" || S == "yes" || S == "on") {
    Out = true;
    return true;
  }
  if (S == "0" || S == "false" || S == "no" || S == "off") {
    Out = false;
    return true;
  }
  return false;
}

void applyEnv(ReportOptions &O) {
  auto Env = llvm::sys::Process::GetEnv("LPTA_OPTIONS");
  if (!Env)
    return;

  std::vector<std::string> Words = splitShellWords(*Env);
  for (size_t I = 0; I < Words.size(); ++I) {
    std::string Token = Words[I];
    if (Token.empty() || Token[0] != '-')
      continue;
    while (!Token.empty() && Token[0] == '-')
      Token.erase(Token.begin());
    if (Token.rfind("lpta-", 0) == 0)
      Token.erase(0, 5);

    std::string Value;
    bool HasValue = false;
    size_t Eq = Token.find('=');
    if (Eq != std::string::npos) {
      Value = Token.substr(Eq + 1);
      Token.erase(Eq);
      HasValue = true;
    }

    const bool IsString =
        Token == "report" || Token == "function" || Token == "pass" ||
        Token == "color" || Token == "out" || Token == "html" ||
        Token == "json" || Token == "compare" || Token == "weights" ||
        Token == "demangle" || Token == "pipeline" || Token == "top" ||
        Token == "min-score";
    if (!HasValue && IsString && I + 1 < Words.size() &&
        Words[I + 1].rfind("-", 0) != 0) {
      Value = Words[++I];
      HasValue = true;
    }

    auto BoolValue = [&](bool &Dest) {
      bool B = true;
      if (HasValue && !parseBool(Value, B)) {
        llvm::errs() << "LPTA warning: invalid boolean for -" << Token << "='"
                     << Value << "'\n";
        return;
      }
      Dest = B;
    };

    if (Token == "report")
      O.view = HasValue ? Value : O.view;
    else if (Token == "top") {
      unsigned N = 10;
      if (HasValue && !llvm::StringRef(Value).getAsInteger(10, N))
        O.top = N;
    } else if (Token == "function")
      O.functionFilter = HasValue ? Value : O.functionFilter;
    else if (Token == "pass")
      O.passFilter = HasValue ? Value : O.passFilter;
    else if (Token == "min-score") {
      if (HasValue)
        O.minScore = std::strtod(Value.c_str(), nullptr);
    } else if (Token == "diff")
      BoolValue(O.diff);
    else if (Token == "color")
      O.color = HasValue ? Value : O.color;
    else if (Token == "out")
      O.out = HasValue ? Value : O.out;
    else if (Token == "html")
      O.html = HasValue ? Value : O.html;
    else if (Token == "json")
      O.json = HasValue ? Value : O.json;
    else if (Token == "compare")
      O.compare = HasValue ? Value : O.compare;
    else if (Token == "weights")
      O.weights = HasValue ? Value : O.weights;
    else if (Token == "demangle")
      O.demangle = HasValue ? Value : O.demangle;
    else if (Token == "pipeline")
      O.pipeline = HasValue ? Value : O.pipeline;
    else if (Token == "skipped")
      BoolValue(O.skipped);
    else
      llvm::errs() << "LPTA warning: unknown LPTA_OPTIONS entry '-" << Token
                   << "' (ignored)\n";
  }
}

void applyCommandLine(ReportOptions &O) {
  if (ClReport.getNumOccurrences())
    O.view = ClReport;
  if (ClTop.getNumOccurrences())
    O.top = ClTop;
  if (ClFunction.getNumOccurrences())
    O.functionFilter = ClFunction;
  if (ClPass.getNumOccurrences())
    O.passFilter = ClPass;
  if (ClMinScore.getNumOccurrences())
    O.minScore = ClMinScore;
  if (ClDiff.getNumOccurrences())
    O.diff = ClDiff;
  if (ClColor.getNumOccurrences())
    O.color = ClColor;
  if (ClOut.getNumOccurrences())
    O.out = ClOut;
  if (ClHtml.getNumOccurrences())
    O.html = ClHtml;
  if (ClJson.getNumOccurrences())
    O.json = ClJson;
  if (ClCompare.getNumOccurrences())
    O.compare = ClCompare;
  if (ClWeights.getNumOccurrences())
    O.weights = ClWeights;
  if (ClDemangle.getNumOccurrences())
    O.demangle = ClDemangle;
  if (ClPipeline.getNumOccurrences())
    O.pipeline = ClPipeline;
  if (ClSkipped.getNumOccurrences())
    O.skipped = ClSkipped;
}

ReportOptions buildOptions() {
  ReportOptions O;
  applyEnv(O);
  applyCommandLine(O);

  if (O.view != "summary" && O.view != "passes" && O.view != "functions" &&
      O.view != "full" && O.view != "off") {
    llvm::errs() << "LPTA warning: unknown report view '" << O.view
                 << "'; using summary\n";
    O.view = "summary";
  }
  if (O.color != "auto" && O.color != "always" && O.color != "never") {
    llvm::errs() << "LPTA warning: unknown color mode '" << O.color
                 << "'; using auto\n";
    O.color = "auto";
  }
  if (O.demangle != "auto" && O.demangle != "off") {
    llvm::errs() << "LPTA warning: unknown demangle mode '" << O.demangle
                 << "'; using auto\n";
    O.demangle = "auto";
  }
  if (!O.weights.empty()) {
    std::string Err;
    if (!loadWeightsFromFile(O.weights, O.scoreWeights, Err))
      llvm::errs() << "LPTA warning: " << Err << "; using built-in weights\n";
  }
  O.compileFilters();
  O.pipeline = detectPipeline(O.pipeline);
  return O;
}

std::vector<std::string> readProcessArgs() {
  std::vector<std::string> Args;
#if defined(__linux__)
  std::ifstream F("/proc/self/cmdline", std::ios::binary);
  if (!F)
    return Args;
  std::string Data((std::istreambuf_iterator<char>(F)),
                   std::istreambuf_iterator<char>());
  size_t Start = 0;
  while (Start < Data.size()) {
    size_t End = Data.find('\0', Start);
    if (End == std::string::npos)
      End = Data.size();
    if (End > Start)
      Args.push_back(Data.substr(Start, End - Start));
    Start = End + 1;
  }
#elif defined(__APPLE__)
  int Argc = 0;
  char **Argv = *_NSGetArgv();
  Argc = *_NSGetArgc();
  for (int I = 0; I < Argc; ++I)
    Args.emplace_back(Argv[I]);
#endif
  return Args;
}

} // namespace

const std::vector<std::string> &getProcessArgs() {
  // Intentionally leaked: reports are emitted from an atexit handler, so
  // function-local statics constructed during pass execution would already be
  // destroyed by the time the handler runs.
  static const std::vector<std::string> *const Args =
      new std::vector<std::string>(readProcessArgs());
  return *Args;
}

std::string getProcessName() {
  const auto &Args = getProcessArgs();
  if (Args.empty())
    return {};
  return llvm::sys::path::filename(Args[0]).str();
}

std::string detectRoute() {
  const auto &Args = getProcessArgs();
  std::string Name = getProcessName();
  if (Name == "opt" || Name.rfind("opt-", 0) == 0)
    return "opt-replay";
  for (const std::string &A : Args)
    if (A == "-load-pass-plugin" || A.rfind("-load-pass-plugin=", 0) == 0)
      return "opt-replay";
  return "in-compiler";
}

std::string detectPipeline(llvm::StringRef Explicit) {
  if (!Explicit.empty())
    return Explicit.str();

  const auto &Args = getProcessArgs();
  std::string Pipeline;
  for (size_t I = 0; I < Args.size(); ++I) {
    const std::string &A = Args[I];
    if (A.rfind("-passes=", 0) == 0) {
      Pipeline = A.substr(8);
    } else if (A == "-passes" && I + 1 < Args.size()) {
      Pipeline = Args[++I];
    }
  }
  if (!Pipeline.empty())
    return Pipeline;

  for (auto It = Args.rbegin(); It != Args.rend(); ++It) {
    const std::string &A = *It;
    if (A == "-O0")
      return "default<O0>";
    if (A == "-O1")
      return "default<O1>";
    if (A == "-O2")
      return "default<O2>";
    if (A == "-O3")
      return "default<O3>";
    if (A == "-Os")
      return "default<Os>";
    if (A == "-Oz")
      return "default<Oz>";
  }
  return "unknown";
}

const ReportOptions &getOptions() {
  // Intentionally leaked: the plugin emits its report from an atexit handler.
  // A function-local static constructed during pass execution would be
  // destroyed before that handler runs, leaving path strings dangling.
  static const ReportOptions *const Opts = new ReportOptions(buildOptions());
  return *Opts;
}

} // namespace lpta
