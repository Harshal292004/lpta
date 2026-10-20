#ifndef LPTA_DIFF_H
#define LPTA_DIFF_H

#include "llvm/ADT/StringRef.h"

#include <string>

namespace lpta {

/// Produce a unified diff between two textual IR snapshots.  Returns an empty
/// string when the inputs are identical.  The implementation uses a bounded
/// Myers diff and falls back to a block replacement for very large changes.
std::string unifiedDiff(llvm::StringRef Before, llvm::StringRef After,
                        llvm::StringRef Label);

} // namespace lpta

#endif
