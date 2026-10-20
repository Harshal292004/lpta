#include "Collector.h"
#include "Version.h"

#include "llvm/Config/llvm-config.h"

#if __has_include(<llvm/Plugins/PassPlugin.h>)
#include <llvm/Plugins/PassPlugin.h>
#else
#include <llvm/Passes/PassPlugin.h>
#endif

#include "llvm/Passes/PassBuilder.h"
#include "llvm/Support/raw_ostream.h"

#include "llvm-c/Core.h"

#include <cstdlib>

extern "C" LLVM_ATTRIBUTE_WEAK ::llvm::PassPluginLibraryInfo
llvmGetPassPluginInfo() {
  // The plugin ABI version is checked by LLVM's loader.  A major-version
  // mismatch can still pass that check, so compare the runtime LLVM version
  // explicitly and fail with a useful message instead of entering an
  // incompatible ABI.
  unsigned Major = 0, Minor = 0, Patch = 0;
  LLVMGetVersion(&Major, &Minor, &Patch);
  if (Major != LLVM_VERSION_MAJOR) {
    llvm::errs() << "LPTA error: plugin built for LLVM " << LLVM_VERSION_MAJOR
                 << " but loaded into LLVM " << Major << "." << Minor << "."
                 << Patch << "; rebuild LPTA against the driver's LLVM.\n";
    return {LLVM_PLUGIN_API_VERSION, "LPTA", LPTA_VERSION_STRING, nullptr};
  }

  return {LLVM_PLUGIN_API_VERSION, "LPTA", LPTA_VERSION_STRING,
          [](llvm::PassBuilder &PB) { lpta::registerCollectorCallbacks(PB); }};
}
