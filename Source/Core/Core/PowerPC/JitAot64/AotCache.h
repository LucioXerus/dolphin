// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <string>
#include <vector>

#include "Common/CommonTypes.h"
#include "Core/PowerPC/PowerPC.h"

namespace Core
{
class System;
}

namespace AotJit
{
// A single ahead-of-time compilation target: the entry point of a block that was
// observed to execute during a previous session. A block is uniquely identified by
// its effective address together with the CPU feature flags (MSR IR/DR + perfmon)
// that were active when it was compiled, matching the JIT's own block keying.
struct AotEntry
{
  u32 effective_address;
  u32 feature_flags;

  friend bool operator==(const AotEntry&, const AotEntry&) = default;
};

// On-disk persistence for the set of AOT compilation targets discovered for a game.
//
// Only PowerPC addresses (never host machine code) are stored, which keeps the cache
// independent of ASLR, the host binary layout, and the exact Dolphin build. A header
// carries a format version and a hash of the JIT settings that affect code generation;
// if either fails to match at load time the cache is discarded rather than misapplied.
class AotCache
{
public:
  // Returns the absolute path of the cache file for the currently running game.
  // Empty if no game-specific identifier is available.
  static std::string GetCacheFilePath(const Core::System& system);

  // Loads the entries for the given file into `out_entries`. Returns true on success.
  // A missing file is not an error: it yields an empty set and returns false so callers
  // can distinguish "nothing cached yet" from "loaded N entries".
  static bool Load(const std::string& path, u32 expected_settings_hash,
                   std::vector<AotEntry>* out_entries);

  // Atomically writes `entries` to `path` (via a temporary file + rename).
  // Returns true on success.
  static bool Save(const std::string& path, u32 settings_hash,
                   const std::vector<AotEntry>& entries);

  // Computes a hash of all JIT settings that influence generated code, so that a cache
  // produced under different settings (fastmem, MMU, FP exceptions, ...) is not reused.
  static u32 ComputeSettingsHash();
};
}  // namespace AotJit
