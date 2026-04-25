// Copyright 2024 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/PowerPC/AOT64/AOTJit64.h"

#include <functional>

#include "Common/Logging/Log.h"
#include "Core/Config/MainSettings.h"
#include "Core/ConfigManager.h"
#include "Core/Core.h"
#include "Core/PowerPC/AOT64/AOTLoader.h"
#include "Core/PowerPC/AOT64/AOTSerializer.h"
#include "Core/PowerPC/Jit64Common/BlockCache.h"
#include "Core/PowerPC/JitInterface.h"
#include "Core/PowerPC/MMU.h"
#include "Core/PowerPC/PPCAnalyst.h"
#include "Core/PowerPC/PPCSymbolDB.h"

AOTJit64::AOTJit64(Core::System& system) : Jit64(system)
{
}

AOTJit64::~AOTJit64() = default;

void AOTJit64::Init()
{
  Jit64::Init();

  m_loader = std::make_unique<AOT64::AOTLoader>();

  m_serialize_enabled = Config::Get(Config::MAIN_AOT64_SERIALIZE);
  if (m_serialize_enabled)
  {
    INFO_LOG_FMT(DYNA_REC, "AOT: Serializer enabled. Blocks will be cached during gameplay.");
  }
}

void AOTJit64::Shutdown()
{
  if (m_serialize_enabled && m_serializer && m_serializer->IsOpen())
  {
    const std::size_t count = m_serializer->GetBlockCount();
    if (count > 0)
    {
      Core::DisplayMessage(fmt::format("AOT: Saving {} blocks to cache...", count), 3000);
      m_serializer->Commit();
      Core::DisplayMessage(fmt::format("AOT: Saved {} blocks to cache.", count), 3000);
    }
  }

  m_loader.reset();
  m_serializer.reset();

  Jit64::Shutdown();
}

void AOTJit64::ClearCache()
{
  Jit64::ClearCache();
}

void AOTJit64::Jit(u32 em_address)
{
  // Lazy initialization: the game ID is only available after boot.
  // On the first JIT compilation, detect the game and set up the serializer/loader.
  if (!m_initialized)
  {
    m_initialized = true;

    if (m_game_id.empty())
    {
      m_game_id = SConfig::GetInstance().GetGameID();
      m_rom_crc32 = std::hash<std::string>{}(m_game_id) & 0xFFFFFFFF;
    }

    if (!m_game_id.empty())
    {
      if (m_serialize_enabled)
      {
        m_serializer = std::make_unique<AOT64::AOTSerializer>(m_rom_crc32, m_game_id);
        m_serializer->Begin();
      }

      if (m_loader && m_loader->CacheExists(m_rom_crc32, m_game_id))
      {
        Core::DisplayMessage("AOT: Loading cache...", 3000);
        const std::size_t loaded =
            m_loader->LoadCache(*this, *GetBlockCache(), m_system, m_rom_crc32, m_game_id);
        if (loaded > 0)
        {
          m_cache_loaded = true;
          Core::DisplayMessage(fmt::format("AOT: Recompiled {} blocks from cache.", loaded), 5000);
        }
        else
        {
          Core::DisplayMessage("AOT: Cache exists but is empty or invalid.", 3000);
        }
      }
      else if (m_serialize_enabled)
      {
        Core::DisplayMessage("AOT: No cache found. Will build during gameplay.", 5000);
      }
    }
  }

  // After loading, check if the requested block was recompiled from cache.
  if (m_cache_loaded)
  {
    const u8* entry = GetBlockCache()->Dispatch();
    if (entry)
      return;  // Block found in cache, dispatcher will handle it.
  }

  // Fall back to normal JIT compilation.
  Jit64::Jit(em_address);

  // Serialize the newly compiled block.
  if (m_serialize_enabled && m_serializer && m_serializer->IsOpen())
  {
    JitBlock* block =
        GetBlockCache()->GetBlockFromStartAddress(em_address, m_ppc_state.feature_flags);
    if (block)
    {
      std::vector<std::pair<u32, u32>> original_insts;
      original_insts.reserve(block->originalSize);
      for (u32 i = 0; i < code_block.m_num_instructions && i < block->originalSize; ++i)
      {
        original_insts.emplace_back(m_code_buffer[i].address, m_code_buffer[i].inst.hex);
      }

      u32 static_gqr_mask = 0;
      std::array<u32, 8> static_gqr_values{};
      if (js.constantGqrValid)
      {
        static_gqr_mask = js.constantGqrValid.m_val;
        for (int gqr = 0; gqr < 8; ++gqr)
        {
          if (js.constantGqrValid[gqr])
            static_gqr_values[gqr] = js.constantGqr[gqr];
        }
      }

      m_serializer->AddBlock(*block, code_block, original_insts, block->near_begin,
                             block->far_begin, js.downcountAmount, static_gqr_mask,
                             static_gqr_values);

      m_blocks_serialized_this_session++;
      if (m_blocks_serialized_this_session >= m_next_progress_report)
      {
        Core::DisplayMessage(
            fmt::format("AOT: Cached {} blocks this session...", m_blocks_serialized_this_session),
            2000);
        m_next_progress_report += 50;
      }
    }
  }
}

void AOTJit64::SetGameInfo(u32 rom_crc32, const std::string& game_id)
{
  m_rom_crc32 = rom_crc32;
  m_game_id = game_id;
}

std::size_t AOTJit64::PrecompileAll()
{
  if (!m_serialize_enabled || !m_serializer)
    return 0;

  const Core::CPUThreadGuard guard(m_system);
  auto& mmu = m_system.GetMMU();

  INFO_LOG_FMT(DYNA_REC, "AOT: Starting proactive compilation for {}", m_game_id);

  PPCSymbolDB temp_db;

  const u32 ram_start = 0x80000000;
  const u32 ram_size = m_system.GetMemory().GetRamSize();
  const u32 ram_end = ram_start + ram_size;

  PPCAnalyst::FindFunctions(guard, ram_start, ram_end, &temp_db);

  if (ram_size <= 0x01800000)
  {
    PPCAnalyst::FindFunctions(guard, 0x00000000, ram_size, &temp_db);
  }

  std::size_t num_functions = 0;
  temp_db.ForEachSymbol([&num_functions](const Common::Symbol& symbol) {
    if (symbol.type == Common::Symbol::Type::Function)
      ++num_functions;
  });

  INFO_LOG_FMT(DYNA_REC, "AOT: Found {} functions to compile", num_functions);

  std::size_t total_blocks = 0;
  temp_db.ForEachSymbol([&](const Common::Symbol& symbol) {
    if (symbol.type != Common::Symbol::Type::Function)
      return;

    static constexpr CPUEmuFeatureFlags flag_combos[] = {
        static_cast<CPUEmuFeatureFlags>(0),
        FEATURE_FLAG_MSR_DR,
        FEATURE_FLAG_MSR_IR,
        static_cast<CPUEmuFeatureFlags>(FEATURE_FLAG_MSR_DR | FEATURE_FLAG_MSR_IR),
    };

    for (CPUEmuFeatureFlags flags : flag_combos)
    {
      m_ppc_state.feature_flags = flags;
      PrecompileRange(symbol.address, symbol.address + symbol.size, flags);
      ++total_blocks;
    }
  });

  for (u32 addr = ram_start; addr < ram_end; addr += 4)
  {
    const auto read_result = mmu.TryReadInstruction(addr);
    if (!read_result.valid)
      continue;

    if (GetBlockCache()->GetBlockFromStartAddress(addr, m_ppc_state.feature_flags))
      continue;

    const UGeckoInstruction inst = read_result.hex;
    if (!PPCTables::IsValidInstruction(inst, addr))
      continue;

    if (!PowerPC::MMU::HostIsRAMAddress(guard, addr))
      continue;

    m_ppc_state.feature_flags =
        static_cast<CPUEmuFeatureFlags>(FEATURE_FLAG_MSR_DR | FEATURE_FLAG_MSR_IR);
    PrecompileRange(addr, addr + 0x1000,
                    static_cast<CPUEmuFeatureFlags>(FEATURE_FLAG_MSR_DR | FEATURE_FLAG_MSR_IR));
  }

  INFO_LOG_FMT(DYNA_REC, "AOT: Proactive compilation complete. {} blocks compiled.",
               m_serializer->GetBlockCount());

  return total_blocks;
}

void AOTJit64::PrecompileRange(u32 start, u32 end, CPUEmuFeatureFlags feature_flags)
{
  auto& mmu = m_system.GetMMU();
  const Core::CPUThreadGuard guard(m_system);

  u32 addr = start;
  while (addr < end)
  {
    if (GetBlockCache()->GetBlockFromStartAddress(addr, feature_flags))
    {
      addr += 4;
      continue;
    }

    const auto read_result = mmu.TryReadInstruction(addr);
    if (!read_result.valid)
    {
      addr += 4;
      continue;
    }

    const UGeckoInstruction inst = read_result.hex;
    if (!PPCTables::IsValidInstruction(inst, addr))
    {
      addr += 4;
      continue;
    }

    if (!PowerPC::MMU::HostIsRAMAddress(guard, addr))
    {
      addr += 4;
      continue;
    }

    m_ppc_state.feature_flags = feature_flags;
    Jit64::Jit(addr);

    JitBlock* block = GetBlockCache()->GetBlockFromStartAddress(addr, feature_flags);
    if (block && m_serializer && m_serializer->IsOpen())
    {
      std::vector<std::pair<u32, u32>> original_insts;
      original_insts.reserve(block->originalSize);
      for (u32 i = 0; i < code_block.m_num_instructions && i < block->originalSize; ++i)
      {
        original_insts.emplace_back(m_code_buffer[i].address, m_code_buffer[i].inst.hex);
      }

      u32 static_gqr_mask = 0;
      std::array<u32, 8> static_gqr_values{};
      if (js.constantGqrValid)
      {
        static_gqr_mask = js.constantGqrValid.m_val;
        for (int gqr = 0; gqr < 8; ++gqr)
        {
          if (js.constantGqrValid[gqr])
            static_gqr_values[gqr] = js.constantGqr[gqr];
        }
      }

      m_serializer->AddBlock(*block, code_block, original_insts, block->near_begin,
                             block->far_begin, js.downcountAmount, static_gqr_mask,
                             static_gqr_values);
    }

    if (block)
      addr += block->originalSize * 4;
    else
      addr += 4;
  }
}
