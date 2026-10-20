#ifndef LPTA_COLLECTOR_H
#define LPTA_COLLECTOR_H

namespace llvm {
class PassBuilder;
}

namespace lpta {

/// Attach the collector to a PassBuilder's instrumentation callbacks.  Safe to
/// call more than once; each callback set is instrumented once.
void registerCollectorCallbacks(llvm::PassBuilder &PB);

/// Flush reports and close output files.  Registered with atexit by the plugin
/// entry point, and idempotent.
void emitReportAtExit();

} // namespace lpta

#endif
