#include "Trace.h"

#include "Version.h"

#include "llvm/Support/FileSystem.h"
#include "llvm/Support/JSON.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/Process.h"
#include "llvm/Support/raw_ostream.h"

#include <cstdio>
#include <cstdlib>
#include <string>

namespace lpta {

namespace {

std::string makeTempPath(llvm::StringRef Path) {
  return (Path + ".tmp." + std::to_string(llvm::sys::Process::getProcessId()))
      .str();
}

bool ensureParent(llvm::StringRef Path, std::string &Err) {
  llvm::SmallString<256> Parent(Path);
  llvm::sys::path::remove_filename(Parent);
  if (Parent.empty())
    return true;
  std::error_code EC = llvm::sys::fs::create_directories(Parent);
  if (EC) {
    Err =
        "cannot create directory '" + Parent.str().str() + "': " + EC.message();
    return false;
  }
  return true;
}

void writeMetricsAttrs(llvm::json::OStream &J, const Metrics &M) {
  J.attribute("insts", M.insts);
  J.attribute("bbs", M.bbs);
  J.attribute("functions", M.functions);
  J.attribute("loads", M.loads);
  J.attribute("stores", M.stores);
  J.attribute("calls", M.calls);
  J.attribute("branches", M.branches);
  J.attribute("phis", M.phis);
  J.attribute("allocas", M.allocas);
  J.attribute("loops", M.loops);
  J.attribute("max_loop_depth", M.maxLoopDepth);
  J.attribute("call_edges", M.callEdges);
}

void writeDeltaAttrs(llvm::json::OStream &J, const MetricsDelta &D) {
  J.attribute("insts", D.insts);
  J.attribute("bbs", D.bbs);
  J.attribute("functions", D.functions);
  J.attribute("loads", D.loads);
  J.attribute("stores", D.stores);
  J.attribute("calls", D.calls);
  J.attribute("branches", D.branches);
  J.attribute("phis", D.phis);
  J.attribute("allocas", D.allocas);
  J.attribute("loops", D.loops);
  J.attribute("max_loop_depth", D.maxLoopDepth);
  J.attribute("call_edges", D.callEdges);
}

bool getString(const llvm::json::Object &O, llvm::StringRef Key,
               std::string &Out) {
  if (auto S = O.getString(Key)) {
    Out = S->str();
    return true;
  }
  return false;
}

uint64_t getUInt(const llvm::json::Object &O, llvm::StringRef Key,
                 uint64_t Default = 0) {
  if (auto V = O.getInteger(Key))
    return static_cast<uint64_t>(*V);
  return Default;
}

int64_t getInt(const llvm::json::Object &O, llvm::StringRef Key,
               int64_t Default = 0) {
  if (auto V = O.getInteger(Key))
    return *V;
  return Default;
}

bool getBool(const llvm::json::Object &O, llvm::StringRef Key,
             bool Default = false) {
  if (auto V = O.getBoolean(Key))
    return *V;
  return Default;
}

Metrics readMetrics(const llvm::json::Object &O) {
  Metrics M;
  M.insts = getUInt(O, "insts");
  M.bbs = getUInt(O, "bbs");
  M.functions = getUInt(O, "functions");
  M.loads = getUInt(O, "loads");
  M.stores = getUInt(O, "stores");
  M.calls = getUInt(O, "calls");
  M.branches = getUInt(O, "branches");
  M.phis = getUInt(O, "phis");
  M.allocas = getUInt(O, "allocas");
  M.loops = getUInt(O, "loops");
  M.maxLoopDepth = getUInt(O, "max_loop_depth");
  M.callEdges = getUInt(O, "call_edges");
  return M;
}

MetricsDelta readDelta(const llvm::json::Object &O) {
  MetricsDelta D;
  D.insts = getInt(O, "insts");
  D.bbs = getInt(O, "bbs");
  D.functions = getInt(O, "functions");
  D.loads = getInt(O, "loads");
  D.stores = getInt(O, "stores");
  D.calls = getInt(O, "calls");
  D.branches = getInt(O, "branches");
  D.phis = getInt(O, "phis");
  D.allocas = getInt(O, "allocas");
  D.loops = getInt(O, "loops");
  D.maxLoopDepth = getInt(O, "max_loop_depth");
  D.callEdges = getInt(O, "call_edges");
  return D;
}

} // namespace

void writeJsonMetrics(llvm::json::OStream &J, const Metrics &M) {
  J.object([&] { writeMetricsAttrs(J, M); });
}

void writeJsonDelta(llvm::json::OStream &J, const MetricsDelta &D) {
  J.object([&] { writeDeltaAttrs(J, D); });
}

JsonTraceWriter::~JsonTraceWriter() {
  if (OS) {
    J.reset();
    OS->close();
  }
}

bool JsonTraceWriter::open(const TraceHeader &Header, llvm::StringRef P,
                           std::string &Err) {
  Path = P.str();
  if (!ensureParent(Path, Err))
    return false;
  TempPath = makeTempPath(Path);

  std::error_code EC;
  OS = std::make_unique<llvm::raw_fd_ostream>(
      TempPath, EC, llvm::sys::fs::CreationDisposition::CD_CreateAlways);
  if (EC) {
    Err = "cannot open '" + TempPath + "': " + EC.message();
    OS.reset();
    return false;
  }
  J = std::make_unique<llvm::json::OStream>(*OS, 2);

  J->objectBegin();
  J->attribute("schema_version", Header.schemaVersion);
  J->attribute("llvm_version", Header.llvmVersion);
  if (!Header.producer.empty())
    J->attribute("producer", Header.producer);
  J->attribute("route", Header.route);
  J->attribute("pipeline", Header.pipeline);
  if (!Header.source.empty())
    J->attribute("source", Header.source);
  if (!Header.targetTriple.empty())
    J->attribute("target_triple", Header.targetTriple);
  if (!Header.lptaVersion.empty())
    J->attribute("lpta_version", Header.lptaVersion);
  J->attributeBegin("events");
  J->arrayBegin();
  return true;
}

bool JsonTraceWriter::write(const Event &E, std::string &Err) {
  if (!J) {
    Err = "JSON trace is not open";
    return false;
  }

  J->object([&] {
    J->attribute("index", E.index);
    J->attribute("pass", E.pass);
    J->attribute("unit_kind", E.unitKind);
    J->attribute("unit_name", E.unitName);
    if (!E.unitDisplayName.empty())
      J->attribute("unit_display_name", E.unitDisplayName);
    if (!E.parentPass.empty())
      J->attribute("parent_pass", E.parentPass);
    if (E.depth != 0)
      J->attribute("depth", E.depth);
    if (!E.affectedFunctions.empty())
      J->attributeArray("affected_functions", [&] {
        for (const std::string &F : E.affectedFunctions)
          J->value(F);
      });
    J->attribute("changed", E.changed);
    if (E.skipped)
      J->attribute("skipped", true);
    if (E.invalidated)
      J->attribute("invalidated", true);
    J->attribute("duration_us", E.durationUs);
    J->attributeObject("before", [&] { writeMetricsAttrs(*J, E.before); });
    J->attributeObject("after", [&] { writeMetricsAttrs(*J, E.after); });
    J->attributeObject("delta", [&] { writeDeltaAttrs(*J, E.delta); });
    J->attribute("score", E.score);

    // Per-function attribution is needed by the offline viewer for module and
    // SCC passes.  Function events already carry the same information in
    // before/after, so omit the redundant array for them.
    if (E.unitKind != "function" && !E.functions.empty()) {
      J->attributeArray("functions", [&] {
        for (const auto &KV : E.functions) {
          const FunctionRecord &R = KV.second;
          MetricsDelta D =
              R.hasDelta ? R.storedDelta : lpta::delta(R.before, R.after);
          J->object([&] {
            J->attribute("name", KV.first);
            J->attribute("before_insts", R.before.insts);
            J->attribute("after_insts", R.after.insts);
            J->attributeObject("delta", [&] { writeDeltaAttrs(*J, D); });
          });
        }
      });
    }

    if (!E.diffRef.empty())
      J->attribute("diff_ref", E.diffRef);
  });
  if (OS->has_error()) {
    Err = "error writing '" + TempPath + "'";
    return false;
  }
  return true;
}

void JsonTraceWriter::abort() {
  J.reset();
  if (OS) {
    OS->close();
    OS.reset();
  }
  if (!TempPath.empty()) {
    llvm::sys::fs::remove(TempPath);
    TempPath.clear();
  }
}

bool JsonTraceWriter::finish(std::string &Err) {
  if (!J)
    return true;
  J->arrayEnd();
  J->attributeEnd();
  J->objectEnd();
  J.reset();
  OS->close();
  bool WriteFailed = OS->has_error();
  OS.reset();

  if (WriteFailed) {
    Err = "error writing '" + TempPath + "'";
    llvm::sys::fs::remove(TempPath);
    return false;
  }

  std::error_code EC = llvm::sys::fs::rename(TempPath, Path);
  if (EC) {
    Err = "cannot rename '" + TempPath + "' to '" + Path + "': " + EC.message();
    llvm::sys::fs::remove(TempPath);
    return false;
  }
  return true;
}

std::string writeSnapshotDiff(llvm::StringRef JsonPath, uint64_t Index,
                              llvm::StringRef Diff, std::string &Err) {
  llvm::SmallString<256> Dir(JsonPath);
  llvm::sys::path::remove_filename(Dir);
  llvm::SmallString<128> Stem(llvm::sys::path::filename(JsonPath));
  llvm::sys::path::replace_extension(Stem, "");
  llvm::SmallString<256> SnapDir(Dir);
  llvm::sys::path::append(SnapDir, Stem + ".snapshots");
  std::error_code EC = llvm::sys::fs::create_directories(SnapDir);
  if (EC) {
    Err = "cannot create snapshot directory: " + EC.message();
    return {};
  }
  llvm::SmallString<256> File(SnapDir);
  llvm::sys::path::append(File, std::to_string(Index) + ".diff");
  if (!writeFileAtomic(File, Diff, Err))
    return {};

  llvm::SmallString<128> Rel(llvm::sys::path::filename(JsonPath));
  llvm::sys::path::replace_extension(Rel, "");
  return (llvm::Twine(Rel) + ".snapshots/" + std::to_string(Index) + ".diff")
      .str();
}

bool readTrace(llvm::StringRef Path, Trace &Out, std::string &Err) {
  auto BufOrErr = llvm::MemoryBuffer::getFile(Path);
  if (!BufOrErr) {
    Err = "cannot read trace '" + Path.str() + "'";
    return false;
  }
  auto Parsed = llvm::json::parse((*BufOrErr)->getBuffer());
  if (!Parsed) {
    Err = "invalid JSON in '" + Path.str() +
          "': " + llvm::toString(Parsed.takeError());
    return false;
  }
  const llvm::json::Object *Root = Parsed->getAsObject();
  if (!Root) {
    Err = "trace root is not a JSON object";
    return false;
  }

  std::string Schema;
  if (!getString(*Root, "schema_version", Schema)) {
    Err = "trace is missing schema_version";
    return false;
  }
  Out.header.schemaVersion = Schema;
  llvm::StringRef MajorText = llvm::StringRef(Schema).split('.').first;
  unsigned Major = 0;
  if (MajorText.getAsInteger(10, Major)) {
    Err = "invalid schema_version '" + Schema + "'";
    return false;
  }
  if (Major != LPTA_TRACE_SCHEMA_MAJOR) {
    Err = "unsupported trace schema major version " + std::to_string(Major) +
          " (this build understands " +
          std::to_string(LPTA_TRACE_SCHEMA_MAJOR) + ".x)";
    return false;
  }

  getString(*Root, "llvm_version", Out.header.llvmVersion);
  getString(*Root, "producer", Out.header.producer);
  getString(*Root, "route", Out.header.route);
  getString(*Root, "pipeline", Out.header.pipeline);
  getString(*Root, "source", Out.header.source);
  getString(*Root, "target_triple", Out.header.targetTriple);
  getString(*Root, "lpta_version", Out.header.lptaVersion);

  const llvm::json::Array *Events = Root->getArray("events");
  if (!Events) {
    Err = "trace is missing the events array";
    return false;
  }

  Out.events.clear();
  Out.events.reserve(Events->size());
  for (const llvm::json::Value &V : *Events) {
    const llvm::json::Object *O = V.getAsObject();
    if (!O) {
      Err = "trace event is not an object";
      return false;
    }
    Event E;
    E.index = getUInt(*O, "index");
    getString(*O, "pass", E.pass);
    getString(*O, "unit_kind", E.unitKind);
    getString(*O, "unit_name", E.unitName);
    getString(*O, "unit_display_name", E.unitDisplayName);
    getString(*O, "parent_pass", E.parentPass);
    E.depth = getUInt(*O, "depth");
    E.changed = getBool(*O, "changed");
    E.skipped = getBool(*O, "skipped");
    E.invalidated = getBool(*O, "invalidated");
    E.durationUs = getUInt(*O, "duration_us");
    if (const llvm::json::Array *Aff = O->getArray("affected_functions"))
      for (const llvm::json::Value &F : *Aff)
        if (auto S = F.getAsString())
          E.affectedFunctions.push_back(S->str());

    if (const llvm::json::Object *B = O->getObject("before"))
      E.before = readMetrics(*B);
    if (const llvm::json::Object *A = O->getObject("after"))
      E.after = readMetrics(*A);
    if (const llvm::json::Object *D = O->getObject("delta"))
      E.delta = readDelta(*D);
    else
      E.delta = lpta::delta(E.before, E.after);
    if (auto S = O->getNumber("score"))
      E.score = *S;

    if (const llvm::json::Array *Funcs = O->getArray("functions")) {
      for (const llvm::json::Value &FV : *Funcs) {
        const llvm::json::Object *FO = FV.getAsObject();
        if (!FO)
          continue;
        std::string Name;
        if (!getString(*FO, "name", Name))
          continue;
        FunctionRecord R;
        R.before.insts = getUInt(*FO, "before_insts");
        R.after.insts = getUInt(*FO, "after_insts");
        if (const llvm::json::Object *D = FO->getObject("delta")) {
          R.storedDelta = readDelta(*D);
          R.hasDelta = true;
        }
        E.functions[Name] = R;
      }
    }

    // For function events the per-function record is implicit.
    if (E.functions.empty() && !E.unitName.empty()) {
      FunctionRecord R;
      R.before = E.before;
      R.after = E.after;
      E.functions[E.unitName] = R;
    }

    getString(*O, "diff_ref", E.diffRef);
    if (!E.diffRef.empty()) {
      llvm::SmallString<256> DiffPath(Path);
      llvm::sys::path::remove_filename(DiffPath);
      llvm::sys::path::append(DiffPath, E.diffRef);
      if (auto DBuf = llvm::MemoryBuffer::getFile(DiffPath))
        E.diff = (*DBuf)->getBuffer().str();
    }

    Out.events.push_back(std::move(E));
  }
  return true;
}

} // namespace lpta
