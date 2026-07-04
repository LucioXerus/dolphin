// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <string>
#include <unordered_set>
#include <vector>

#include "Common/CommonTypes.h"
#include "Core/PowerPC/Jit64/Jit.h"
#include "Core/PowerPC/JitAot64/AotCache.h"

namespace Core
{
class System;
}

// Ahead-of-Time recompiler for x86-64.
//
// JitAot64 reuses the entire Jit64 compilation pipeline unchanged. Its only additions are:
//   * Discovery: every block the JIT compiles is recorded as an (address, feature_flags) pair.
//   * Persistence: that set is stored per-game on disk, so it survives across sessions and
//     converges toward covering all reachable code as the game is played.
//   * Warmup: before the game's first instruction runs, all previously discovered blocks are
//     recompiled up-front behind a progress bar, so that steady-state gameplay incurs no
//     recompilation cost.
//
// Because only PowerPC addresses are persisted (never host machine code), the cache is immune to
// ASLR and host-binary differences. Code that first appears at runtime (streamed from the disc,
// reached via an indirect branch, etc.) is still compiled on demand by the underlying JIT and
// folded into the discovery set for the next launch.
class JitAot64 final : public Jit64
{
public:
  explicit JitAot64(Core::System& system);
  ~JitAot64() override;

  void Shutdown() override;

  void Jit(u32 em_address) override;

  void Run() override;

  const char* GetName() const override { return "AOT JIT64"; }

private:
  // Runs the ahead-of-time compilation pass for all discovered blocks, driving the on-screen
  // progress bar. Safe to call multiple times; only the first call per session does work.
  void PrecompileDiscoveredBlocks();

  // Records a compiled block for persistence. Called from Jit().
  void RecordDiscovered(u32 effective_address, u32 feature_flags);

  // Loads/saves the on-disk discovery set for the current game.
  void LoadDiscovered();
  void SaveDiscovered();

  // Packs an (address, feature_flags) pair into a single 64-bit key for set membership.
  static u64 MakeKey(u32 effective_address, u32 feature_flags);

  std::string m_cache_path;
  u32 m_settings_hash = 0;

  // All discovered blocks, deduplicated. Kept as both a fast membership set and an insertion-ordered
  // list so persistence preserves discovery order (which keeps warmup roughly in execution order).
  std::unordered_set<u64> m_discovered_keys;
  std::vector<AotJit::AotEntry> m_discovered;

  // Whether m_discovered has gained entries since the last save.
  bool m_discovered_dirty = false;

  // Guards the one-time warmup pass and suppresses discovery recording during it.
  bool m_precompiled_this_session = false;
  bool m_in_precompile = false;
};
