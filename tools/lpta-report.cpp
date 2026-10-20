#include "Report.h"
#include "Trace.h"

#include "llvm/Support/Process.h"
#include "llvm/Support/raw_ostream.h"

#include <cstdlib>
#include <string>
#include <vector>

namespace {

void usage() {
  llvm::errs()
      << "usage: lpta-report [options] <trace.json>\n"
      << "       lpta-report diff <base.json> <current.json> [options]\n\n"
      << "options:\n"
      << "  --view=summary|passes|functions|full  terminal view (default "
         "summary)\n"
      << "  --top=N                              rows in ranked tables (0 = "
         "all)\n"
      << "  --function=NAME|REGEX                function filter\n"
      << "  --pass=NAME|REGEX                    pass filter\n"
      << "  --min-score=X                        minimum event score\n"
      << "  --diff                               show captured IR diffs\n"
      << "  --color=auto|always|never            ANSI color control\n"
      << "  --demangle=auto|off                  function name demangling\n"
      << "  -o FILE, --output=FILE               write the report to FILE\n";
}

bool startsWith(llvm::StringRef S, llvm::StringRef Prefix) {
  return S.starts_with(Prefix);
}

bool takeValue(llvm::StringRef Arg, llvm::StringRef Name, std::string &Out) {
  if (Arg == Name) {
    Out.clear();
    return false;
  }
  if (startsWith(Arg, (Name + "=").str())) {
    Out = Arg.drop_front(Name.size() + 1).str();
    return true;
  }
  return false;
}

} // namespace

int main(int argc, char **argv) {
  lpta::ReportOptions Opts;
  std::string Output;
  bool ShowHelp = false;
  std::vector<std::string> Positional;

  for (int I = 1; I < argc; ++I) {
    llvm::StringRef A(argv[I]);
    std::string V;
    if (A == "--help" || A == "-h") {
      ShowHelp = true;
    } else if (takeValue(A, "--view", V) || takeValue(A, "-view", V)) {
      Opts.view = V;
    } else if (takeValue(A, "--top", V) || takeValue(A, "-top", V)) {
      unsigned N = 10;
      if (V.empty() || llvm::StringRef(V).getAsInteger(10, N)) {
        llvm::errs() << "lpta-report: invalid --top value '" << V << "'\n";
        return 2;
      }
      Opts.top = N;
    } else if (takeValue(A, "--function", V) || takeValue(A, "-function", V)) {
      Opts.functionFilter = V;
    } else if (takeValue(A, "--pass", V) || takeValue(A, "-pass", V)) {
      Opts.passFilter = V;
    } else if (takeValue(A, "--min-score", V) ||
               takeValue(A, "-min-score", V)) {
      Opts.minScore = std::strtod(V.c_str(), nullptr);
    } else if (A == "--diff") {
      Opts.diff = true;
    } else if (takeValue(A, "--color", V) || takeValue(A, "-color", V)) {
      Opts.color = V;
    } else if (takeValue(A, "--demangle", V) || takeValue(A, "-demangle", V)) {
      Opts.demangle = V;
    } else if (takeValue(A, "--output", V) || takeValue(A, "-o", V)) {
      Output = V;
    } else if (A == "-o" || A == "--output") {
      if (I + 1 >= argc) {
        llvm::errs() << "lpta-report: missing value for " << A << "\n";
        return 2;
      }
      Output = argv[++I];
    } else if (startsWith(A, "-")) {
      llvm::errs() << "lpta-report: unknown option '" << A << "'\n";
      usage();
      return 2;
    } else {
      Positional.push_back(A.str());
    }
  }

  if (ShowHelp) {
    usage();
    return 0;
  }
  if (Opts.view != "summary" && Opts.view != "passes" &&
      Opts.view != "functions" && Opts.view != "full") {
    llvm::errs() << "lpta-report: unknown view '" << Opts.view << "'\n";
    return 2;
  }
  if (Opts.color != "auto" && Opts.color != "always" && Opts.color != "never") {
    llvm::errs() << "lpta-report: unknown color mode '" << Opts.color << "'\n";
    return 2;
  }
  if (Opts.demangle != "auto" && Opts.demangle != "off") {
    llvm::errs() << "lpta-report: unknown demangle mode '" << Opts.demangle
                 << "'\n";
    return 2;
  }
  Opts.out = Output;
  if (Opts.color == "auto") {
    bool TTY = llvm::sys::Process::StandardOutHasColors();
    bool NoColor = llvm::sys::Process::GetEnv("NO_COLOR").has_value();
    Opts.color = (TTY && !NoColor && Output.empty()) ? "always" : "never";
  }
  Opts.compileFilters();

  bool CompareMode = Positional.size() == 3 && Positional[0] == "diff";
  if (CompareMode) {
    lpta::Trace Base, Current;
    std::string Err;
    if (!lpta::readTrace(Positional[1], Base, Err)) {
      llvm::errs() << "lpta-report: " << Err << "\n";
      return 1;
    }
    if (!lpta::readTrace(Positional[2], Current, Err)) {
      llvm::errs() << "lpta-report: " << Err << "\n";
      return 1;
    }
    std::string Text = lpta::compareTraces(Base, Current, Opts.top);
    if (Output.empty()) {
      llvm::outs() << Text;
    } else if (!lpta::writeFileAtomic(Output, Text, Err)) {
      llvm::errs() << "lpta-report: " << Err << "\n";
      return 1;
    }
    return 0;
  }

  if (Positional.size() != 1) {
    usage();
    return 2;
  }

  lpta::Trace T;
  std::string Err;
  if (!lpta::readTrace(Positional[0], T, Err)) {
    llvm::errs() << "lpta-report: " << Err << "\n";
    return 1;
  }

  lpta::Aggregator Agg(Opts);
  for (const lpta::Event &E : T.events)
    Agg.add(E);

  std::string Text = lpta::renderTerminal(T.header, Agg, Opts);
  if (Output.empty()) {
    llvm::outs() << Text;
  } else if (!lpta::writeFileAtomic(Output, Text, Err)) {
    llvm::errs() << "lpta-report: " << Err << "\n";
    return 1;
  }
  return 0;
}
