// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/PowerPC/JitAot64/JitAot64.h"

#include <utility>
#include <vector>

#include <fmt/format.h>

#include "Common/CommonTypes.h"
#include "Common/Config/Config.h"
#include "Common/Logging/Log.h"
#include "Common/MsgHandler.h"

#include "Core/Config/MainSettings.h"
#include "Core/Core.h"
#include "Core/PowerPC/Gekko.h"
#include "Core/PowerPC/JitInterface.h"
#include "Core/PowerPC/MMU.h"
#include "Core/PowerPC/PowerPC.h"
#include "Core/System.h"

#include "VideoCommon/Present.h"

// How often (in blocks) the progress bar is redrawn during the warmup pass. Presenting is far more
// expensive than compiling a single small block, so we batch updates.
static constexpr size_t PROGRESS_UPDATE_INTERVAL = 256;

JitAot64::JitAot64(Core::System& system) : Jit64(system)
{
}

JitAot64::~JitAot64() = default;

u64 JitAot64::MakeKey(u32 effective_address, u32 feature_flags)
{
  return (static_cast<u64>(feature_flags) << 32) | effective_address;
}

void JitAot64::Jit(u32 em_address)
{
  Jit64::Jit(em_address);

  // During the warmup pass every target already lives in m_discovered, so there is nothing to
  // record and we avoid mutating the list we are iterating over.
  if (m_in_precompile)
    return;

  // Only remember blocks that actually compiled. GetBlockFromStartAddress performs the same lookup
  // the dispatcher uses, so a hit means the block is installed and will be found again next launch.
  const CPUEmuFeatureFlags feature_flags = m_ppc_state.feature_flags;
  if (GetBlockCache()->GetBlockFromStartAddress(em_address, feature_flags))
    RecordDiscovered(em_address, static_cast<u32>(feature_flags));
}

void JitAot64::Run()
{
  PrecompileDiscoveredBlocks();
  Jit64::Run();
}

void JitAot64::Shutdown()
{
  SaveDiscovered();
  Jit64::Shutdown();
}

void JitAot64::RecordDiscovered(u32 effective_address, u32 feature_flags)
{
  if (m_discovered_keys.insert(MakeKey(effective_address, feature_flags)).second)
  {
    m_discovered.push_back(AotJit::AotEntry{effective_address, feature_flags});
    m_discovered_dirty = true;
  }
}

void JitAot64::LoadDiscovered()
{
  std::vector<AotJit::AotEntry> entries;
  AotJit::AotCache::Load(m_cache_path, m_settings_hash, &entries);

  m_discovered.reserve(entries.size());
  for (const AotJit::AotEntry& entry : entries)
  {
    if (m_discovered_keys.insert(MakeKey(entry.effective_address, entry.feature_flags)).second)
      m_discovered.push_back(entry);
  }
  m_discovered_dirty = false;
}

void JitAot64::SaveDiscovered()
{
  if (!m_discovered_dirty || m_cache_path.empty() || m_discovered.empty())
    return;

  if (AotJit::AotCache::Save(m_cache_path, m_settings_hash, m_discovered))
    m_discovered_dirty = false;
}

void JitAot64::PrecompileDiscoveredBlocks()
{
  if (m_precompiled_this_session)
    return;
  m_precompiled_this_session = true;

  // The game is fully booted by the time the CPU core first runs, so the per-game identifier and
  // the initial code are now available. Resolve the cache path and load the discovery set here
  // rather than in Init(), where the game ID may not be known yet.
  m_settings_hash = AotJit::AotCache::ComputeSettingsHash();
  m_cache_path = AotJit::AotCache::GetCacheFilePath(m_system);
  LoadDiscovered();

  if (!Config::Get(Config::MAIN_PRECOMPILE_POWERPC) || m_discovered.empty())
    return;

  INFO_LOG_FMT(DYNA_REC, "AOT: precompiling {} discovered PowerPC blocks.", m_discovered.size());

  // Save the CPU state we temporarily perturb to reproduce each block's compilation context.
  const u32 saved_pc = m_ppc_state.pc;
  const u32 saved_npc = m_ppc_state.npc;
  const u32 saved_msr = m_ppc_state.msr.Hex;
  const CPUEmuFeatureFlags saved_feature_flags = m_ppc_state.feature_flags;
  const u32 saved_exceptions = m_ppc_state.Exceptions;
  const int saved_downcount = m_ppc_state.downcount;

  const Core::CPUThreadGuard guard(m_system);
  const std::string title = Common::GetStringT("Recompiling PowerPC");

  // Iterate a snapshot: Jit() must not append to m_discovered while we walk it (it won't, thanks to
  // m_in_precompile, but a snapshot keeps this robust against future changes).
  const std::vector<AotJit::AotEntry> targets = m_discovered;
  const size_t total = targets.size();
  size_t processed = 0;
  size_t compiled = 0;

  m_in_precompile = true;
  for (const AotJit::AotEntry& entry : targets)
  {
    const auto feature_flags = static_cast<CPUEmuFeatureFlags>(entry.feature_flags);

    // Recreate the translation/keying state under which the block was originally compiled.
    m_ppc_state.feature_flags = feature_flags;
    m_ppc_state.msr.IR = (entry.feature_flags & FEATURE_FLAG_MSR_IR) != 0;
    m_ppc_state.msr.DR = (entry.feature_flags & FEATURE_FLAG_MSR_DR) != 0;
    m_ppc_state.pc = entry.effective_address;
    m_ppc_state.npc = entry.effective_address;

    if (!GetBlockCache()->GetBlockFromStartAddress(entry.effective_address, feature_flags))
    {
      // Only compile code that is resident and translatable right now. Code that isn't yet in
      // memory (e.g. an overlay the game hasn't loaded) would make the analyzer raise an ISI
      // exception; such blocks are left to be compiled on demand when they actually execute, and
      // will be recorded again for a future, more complete warmup.
      if (PowerPC::MMU::HostIsInstructionRAMAddress(guard, entry.effective_address,
                                                    PowerPC::RequestedAddressSpace::Effective))
      {
        Jit(entry.effective_address);
        ++compiled;
      }
    }

    if (++processed % PROGRESS_UPDATE_INTERVAL == 0)
    {
      VideoCommon::DrawImmediateProgressBar(
          title, fmt::format("Recompiling PowerPC: {}/{}", processed, total),
          static_cast<float>(processed) / static_cast<float>(total));
    }
  }
  m_in_precompile = false;

  // Restore the CPU state exactly as we found it before execution begins.
  m_ppc_state.pc = saved_pc;
  m_ppc_state.npc = saved_npc;
  m_ppc_state.msr.Hex = saved_msr;
  m_ppc_state.feature_flags = saved_feature_flags;
  m_ppc_state.Exceptions = saved_exceptions;
  m_ppc_state.downcount = saved_downcount;

  // Toggling MSR.DR above may have left mem_ptr pointing at the wrong base; put it back.
  m_system.GetJitInterface().UpdateMembase();

  // A final full-progress present clears the bar before the game's first frame.
  VideoCommon::DrawImmediateProgressBar(title, fmt::format("Recompiling PowerPC: {}/{}", total, total),
                                        1.0f);

  INFO_LOG_FMT(DYNA_REC, "AOT: compiled {} of {} blocks ahead of time ({} deferred to runtime).",
               compiled, total, total - compiled);
}
