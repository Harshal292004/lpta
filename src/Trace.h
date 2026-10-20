#ifndef LPTA_TRACE_H
#define LPTA_TRACE_H

#include "Report.h"

#include "llvm/ADT/StringRef.h"
#include "llvm/Support/JSON.h"
#include "llvm/Support/raw_ostream.h"

#include <memory>
#include <string>

namespace lpta {

/// Streams a versioned trace to a temporary file and renames it into place on
/// finish.  Only one event is held in memory at a time.
class JsonTraceWriter {
public:
  JsonTraceWriter() = default;
  ~JsonTraceWriter();

  bool open(const TraceHeader &Header, llvm::StringRef Path, std::string &Err);
  bool write(const Event &E, std::string &Err);
  bool finish(std::string &Err);
  /// Close and delete a partially written trace.
  void abort();
  bool isOpen() const { return OS != nullptr; }

private:
  std::string Path;
  std::string TempPath;
  std::unique_ptr<llvm::raw_fd_ostream> OS;
  std::unique_ptr<llvm::json::OStream> J;
};

/// Read and validate a trace.  Unknown major schema versions are rejected.
bool readTrace(llvm::StringRef Path, Trace &Out, std::string &Err);

/// Write a diff beside a JSON trace and return its path relative to the trace
/// directory, or an empty string on failure.
std::string writeSnapshotDiff(llvm::StringRef JsonPath, uint64_t Index,
                              llvm::StringRef Diff, std::string &Err);

void writeJsonMetrics(llvm::json::OStream &J, const Metrics &M);
void writeJsonDelta(llvm::json::OStream &J, const MetricsDelta &D);

} // namespace lpta

#endif
