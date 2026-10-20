#ifndef LPTA_OPTIONS_H
#define LPTA_OPTIONS_H

#include "Report.h"

#include <string>
#include <vector>

namespace lpta {

/// Options are collected lazily: by the time the first pass callback runs the
/// host driver has parsed its command line.  Command-line flags take
/// precedence over LPTA_OPTIONS, which takes precedence over built-in values.
const ReportOptions &getOptions();

const std::vector<std::string> &getProcessArgs();
std::string getProcessName();
std::string detectRoute();
std::string detectPipeline(llvm::StringRef Explicit = {});

} // namespace lpta

#endif
