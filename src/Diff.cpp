#include "Diff.h"

#include "llvm/ADT/SmallVector.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>
#include <cstddef>
#include <string>
#include <vector>

namespace lpta {

namespace {

enum class EditKind { Equal, Delete, Insert };

struct Edit {
  EditKind Kind;
  std::string Line;
};

std::vector<std::string> splitLines(llvm::StringRef Text) {
  std::vector<std::string> Lines;
  // Keep empty lines, but do not invent a trailing empty line for the final
  // newline (the usual text-file convention).
  size_t Start = 0;
  while (Start < Text.size()) {
    size_t End = Text.find('\n', Start);
    if (End == llvm::StringRef::npos)
      End = Text.size();
    Lines.push_back(Text.substr(Start, End - Start).str());
    Start = End + 1;
  }
  return Lines;
}

void fallbackEdits(const std::vector<std::string> &A,
                   const std::vector<std::string> &B, std::vector<Edit> &Out) {
  size_t Prefix = 0;
  while (Prefix < A.size() && Prefix < B.size() && A[Prefix] == B[Prefix])
    ++Prefix;

  size_t Suffix = 0;
  while (Suffix + Prefix < A.size() && Suffix + Prefix < B.size() &&
         A[A.size() - 1 - Suffix] == B[B.size() - 1 - Suffix])
    ++Suffix;

  for (size_t I = 0; I < Prefix; ++I)
    Out.push_back({EditKind::Equal, A[I]});
  for (size_t I = Prefix; I < A.size() - Suffix; ++I)
    Out.push_back({EditKind::Delete, A[I]});
  for (size_t I = Prefix; I < B.size() - Suffix; ++I)
    Out.push_back({EditKind::Insert, B[I]});
  for (size_t I = A.size() - Suffix; I < A.size(); ++I)
    Out.push_back({EditKind::Equal, A[I]});
}

/// Bounded Myers diff.  Returns false when the edit distance is too large.
bool myersEdits(const std::vector<std::string> &A,
                const std::vector<std::string> &B, std::vector<Edit> &Out) {
  const int N = static_cast<int>(A.size());
  const int M = static_cast<int>(B.size());
  const int MaxD = 1200;
  const int Max = std::min(N + M, MaxD);
  if (N == 0 || M == 0) {
    for (const std::string &S : A)
      Out.push_back({EditKind::Delete, S});
    for (const std::string &S : B)
      Out.push_back({EditKind::Insert, S});
    return true;
  }

  const int Offset = MaxD + 1;
  const int Width = 2 * Offset + 3;
  std::vector<int> V(Width, 0);
  std::vector<std::vector<int>> Trace;
  Trace.reserve(static_cast<size_t>(Max) + 1);

  int FoundD = -1;
  for (int D = 0; D <= Max && FoundD < 0; ++D) {
    Trace.push_back(V);
    for (int K = -D; K <= D; K += 2) {
      int X;
      if (K == -D || (K != D && V[Offset + K - 1] < V[Offset + K + 1]))
        X = V[Offset + K + 1];
      else
        X = V[Offset + K - 1] + 1;
      int Y = X - K;
      while (X < N && Y < M && X >= 0 && Y >= 0 && A[X] == B[Y]) {
        ++X;
        ++Y;
      }
      V[Offset + K] = X;
      if (X >= N && Y >= M) {
        FoundD = D;
        break;
      }
    }
  }

  if (FoundD < 0)
    return false;

  std::vector<Edit> Reversed;
  int X = N;
  int Y = M;
  for (int D = FoundD; D > 0; --D) {
    const std::vector<int> &Prev = Trace[D];
    const int K = X - Y;
    int PrevK;
    if (K == -D || (K != D && Prev[Offset + K - 1] < Prev[Offset + K + 1]))
      PrevK = K + 1;
    else
      PrevK = K - 1;

    const int PrevX = Prev[Offset + PrevK];
    const int PrevY = PrevX - PrevK;

    while (X > PrevX && Y > PrevY) {
      --X;
      --Y;
      Reversed.push_back({EditKind::Equal, A[X]});
    }
    if (X == PrevX) {
      --Y;
      Reversed.push_back({EditKind::Insert, B[Y]});
    } else {
      --X;
      Reversed.push_back({EditKind::Delete, A[X]});
    }
  }
  while (X > 0 && Y > 0) {
    --X;
    --Y;
    Reversed.push_back({EditKind::Equal, A[X]});
  }
  while (X > 0) {
    --X;
    Reversed.push_back({EditKind::Delete, A[X]});
  }
  while (Y > 0) {
    --Y;
    Reversed.push_back({EditKind::Insert, B[Y]});
  }

  Out.insert(Out.end(), Reversed.rbegin(), Reversed.rend());
  return true;
}

} // namespace

std::string unifiedDiff(llvm::StringRef Before, llvm::StringRef After,
                        llvm::StringRef Label) {
  if (Before == After)
    return {};

  std::vector<std::string> A = splitLines(Before);
  std::vector<std::string> B = splitLines(After);

  std::vector<Edit> Edits;
  if (!myersEdits(A, B, Edits)) {
    Edits.clear();
    fallbackEdits(A, B, Edits);
  }

  bool AnyChange = false;
  for (const Edit &E : Edits)
    if (E.Kind != EditKind::Equal) {
      AnyChange = true;
      break;
    }
  if (!AnyChange)
    return {};

  // oldAt/newAt contain the 1-based line number at the beginning of each edit.
  std::vector<size_t> OldAt(Edits.size() + 1, 1);
  std::vector<size_t> NewAt(Edits.size() + 1, 1);
  for (size_t I = 0; I < Edits.size(); ++I) {
    OldAt[I + 1] = OldAt[I] + (Edits[I].Kind == EditKind::Equal ||
                                       Edits[I].Kind == EditKind::Delete
                                   ? 1
                                   : 0);
    NewAt[I + 1] = NewAt[I] + (Edits[I].Kind == EditKind::Equal ||
                                       Edits[I].Kind == EditKind::Insert
                                   ? 1
                                   : 0);
  }

  constexpr size_t Context = 3;
  std::string Result;
  llvm::raw_string_ostream OS(Result);
  OS << "--- before/" << Label << "\n";
  OS << "+++ after/" << Label << "\n";

  size_t Pos = 0;
  while (Pos < Edits.size()) {
    while (Pos < Edits.size() && Edits[Pos].Kind == EditKind::Equal)
      ++Pos;
    if (Pos == Edits.size())
      break;

    size_t Start = Pos > Context ? Pos - Context : 0;
    size_t LastChange = Pos;
    size_t Scan = Pos;
    while (Scan < Edits.size()) {
      if (Edits[Scan].Kind != EditKind::Equal)
        LastChange = Scan;
      if (Scan > LastChange + 2 * Context)
        break;
      ++Scan;
    }
    size_t End = std::min(Edits.size(), LastChange + Context + 1);

    size_t OldStart = OldAt[Start];
    size_t NewStart = NewAt[Start];
    size_t OldCount = 0;
    size_t NewCount = 0;
    for (size_t I = Start; I < End; ++I) {
      if (Edits[I].Kind != EditKind::Insert)
        ++OldCount;
      if (Edits[I].Kind != EditKind::Delete)
        ++NewCount;
    }
    // Standard convention for empty ranges: refer to the previous line,
    // or line zero at the start of the file.
    if (OldCount == 0)
      OldStart = OldStart > 1 ? OldStart - 1 : 0;
    if (NewCount == 0)
      NewStart = NewStart > 1 ? NewStart - 1 : 0;

    OS << "@@ -" << OldStart << "," << OldCount << " +" << NewStart << ","
       << NewCount << " @@\n";
    for (size_t I = Start; I < End; ++I) {
      const Edit &E = Edits[I];
      switch (E.Kind) {
      case EditKind::Equal:
        OS << ' ' << E.Line << '\n';
        break;
      case EditKind::Delete:
        OS << '-' << E.Line << '\n';
        break;
      case EditKind::Insert:
        OS << '+' << E.Line << '\n';
        break;
      }
      if (OS.tell() > 4 * 1024 * 1024) {
        OS << "... diff truncated (larger than 4 MiB) ...\n";
        Pos = Edits.size();
        break;
      }
    }
    Pos = End;
  }
  return OS.str();
}

} // namespace lpta
