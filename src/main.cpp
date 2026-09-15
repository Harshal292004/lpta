#include "instrumentor.h"

#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/PassManager.h"
#include "llvm/IRReader/IRReader.h"
#include "llvm/MC/TargetRegistry.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Support/CodeGen.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/InitLLVM.h"
#include "llvm/Support/SourceMgr.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Target/TargetMachine.h"
#include "llvm/Target/TargetOptions.h"
#include "llvm/TargetParser/Host.h"
#include "llvm/TargetParser/Triple.h"

#include <memory>
#include <optional>

using namespace llvm;

static cl::opt<std::string> InputFile(cl::Positional, cl::desc("<input IR file>"),
                                      cl::Required);
static cl::opt<std::string> Pipeline("passes",
                                     cl::desc("Pipeline to run, e.g. default<O2>"),
                                     cl::init("default<O2>"));
static cl::opt<std::string> OutputPath("o", cl::desc("Output report path"),
                                       cl::init("out/report.html"));

int main(int argc, char **argv) {
  InitLLVM X(argc, argv);
  cl::ParseCommandLineOptions(argc, argv,
                              "LPTA - LLVM Pass Transformation Analyzer\n");

  InitializeAllTargets();
  InitializeAllTargetMCs();
  InitializeAllAsmPrinters();
  InitializeAllAsmParsers();

  LLVMContext Ctx;
  SMDiagnostic Err;
  std::unique_ptr<Module> M = parseIRFile(InputFile, Err, Ctx);
  if (!M) {
    Err.print("lpta", errs());
    return 1;
  }

  Triple TheTriple = M->getTargetTriple();
  if (TheTriple.getTriple().empty())
    TheTriple = Triple(sys::getDefaultTargetTriple());

  std::string ErrorMsg;
  const Target *TheTarget = TargetRegistry::lookupTarget(TheTriple, ErrorMsg);
  if (!TheTarget) {
    errs() << "No target for triple '" << TheTriple.getTriple() << "': " << ErrorMsg << "\n";
    return 1;
  }

  TargetOptions Opts;
  std::optional<Reloc::Model> RM;
  std::unique_ptr<TargetMachine> TM(
      TheTarget->createTargetMachine(TheTriple, "generic", "", Opts, RM));

  lpta::PipelineInstrumentor Inst;
  PassBuilder PB(TM.get(), PipelineTuningOptions(), std::nullopt,
                 &Inst.callbacks());

  LoopAnalysisManager LAM;
  FunctionAnalysisManager FAM;
  CGSCCAnalysisManager CGAM;
  ModuleAnalysisManager MAM;
  PB.registerModuleAnalyses(MAM);
  PB.registerCGSCCAnalyses(CGAM);
  PB.registerFunctionAnalyses(FAM);
  PB.registerLoopAnalyses(LAM);
  PB.crossRegisterProxies(LAM, FAM, CGAM, MAM);

  Inst.registerCallbacks(PB);

  ModulePassManager MPM;
  if (Error PipeErr = PB.parsePassPipeline(MPM, Pipeline)) {
    errs() << "Failed to parse pipeline '" << Pipeline << "': "
           << toString(std::move(PipeErr)) << "\n";
    return 1;
  }

  errs() << "\n=== Running pipeline: " << Pipeline << " ===\n";
  MPM.run(*M, MAM);

  Inst.printSummary();
  errs() << "Report target: " << OutputPath << " (report built in a later milestone)\n";

  return 0;
}