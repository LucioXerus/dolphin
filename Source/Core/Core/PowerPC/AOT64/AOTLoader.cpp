// Copyright 2024 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/PowerPC/AOT64/AOTLoader.h"

#include <cstring>
#include <fstream>

#include "Common/Align.h"
#include "Common/FileUtil.h"
#include "Common/Logging/Log.h"
#include "Common/MemoryUtil.h"
#include "Core/Core.h"
#include "Core/PowerPC/Jit64/Jit.h"
#include "Core/PowerPC/Jit64Common/BlockCache.h"
#include "Core/PowerPC/JitCommon/JitCache.h"
#include "Core/PowerPC/MMU.h"
#include "Core/PowerPC/PowerPC.h"
#include "Core/System.h"

namespace AOT64
{

AOTLoader::AOTLoader() = default;
AOTLoader::~AOTLoader() = default;

bool AOTLoader::CacheExists(u32 rom_crc32, const std::string& game_id) const
{
  return File::Exists(BuildAOTCachePath(rom_crc32, game_id));
}

std::size_t AOTLoader::LoadCache(Jit64& jit, JitBlockCache& block_cache, Core::System& system,
                                 u32 rom_crc32, const std::string& game_id)
{
  const std::string path = BuildAOTCachePath(rom_crc32, game_id);
  if (!File::Exists(path))
    return 0;

  std::ifstream file(path, std::ios::binary);
  if (!file)
  {
    WARN_LOG_FMT(DYNA_REC, "AOT: Failed to open {} for reading", path);
    return 0;
  }

  // Read file header
  AOTFileHeader file_header;
  file.read(reinterpret_cast<char*>(&file_header), sizeof(file_header));
  if (file.gcount() != sizeof(file_header))
  {
    WARN_LOG_FMT(DYNA_REC, "AOT: Failed to read header from {}", path);
    return 0;
  }

  if (file_header.magic != AOT_FILE_MAGIC)
  {
    WARN_LOG_FMT(DYNA_REC, "AOT: Invalid magic in {}", path);
    return 0;
  }

  if (file_header.version != AOT_FILE_VERSION)
  {
    WARN_LOG_FMT(DYNA_REC, "AOT: Version mismatch in {} (expected {}, got {})", path,
                 AOT_FILE_VERSION, file_header.version);
    return 0;
  }

  if (file_header.rom_crc32 != rom_crc32)
  {
    WARN_LOG_FMT(DYNA_REC, "AOT: ROM CRC mismatch in {} (expected {:08x}, got {:08x})", path,
                 rom_crc32, file_header.rom_crc32);
    return 0;
  }

  Core::DisplayMessage(fmt::format("AOT: Recompiling {} cached blocks...", file_header.block_count), 5000);

  std::size_t loaded_count = 0;
  std::size_t validated_count = 0;

  for (u32 i = 0; i < file_header.block_count; ++i)
  {
    // Read block header
    AOTBlockHeader block_header;
    file.read(reinterpret_cast<char*>(&block_header), sizeof(block_header));
    if (file.gcount() != sizeof(block_header))
    {
      WARN_LOG_FMT(DYNA_REC, "AOT: Failed to read block header {} from {}", i, path);
      break;
    }

    // Read metadata (skip - not needed for recompilation)
    std::vector<AOTLinkData> link_data(block_header.num_link_data);
    if (!link_data.empty())
    {
      file.read(reinterpret_cast<char*>(link_data.data()),
                link_data.size() * sizeof(AOTLinkData));
    }

    std::vector<AOTPhysicalRange> physical_ranges(block_header.num_physical_ranges);
    if (!physical_ranges.empty())
    {
      file.read(reinterpret_cast<char*>(physical_ranges.data()),
                physical_ranges.size() * sizeof(AOTPhysicalRange));
    }

    std::vector<AOTOriginalInstruction> original_instructions;
    if (block_header.original_buffer_size > 0)
    {
      const std::size_t num_insts = block_header.original_buffer_size / sizeof(AOTOriginalInstruction);
      original_instructions.resize(num_insts);
      file.read(reinterpret_cast<char*>(original_instructions.data()), block_header.original_buffer_size);
    }

    // Skip serialized x64 code (we will recompile, not copy)
    if (block_header.near_code_size > 0)
      file.seekg(block_header.near_code_size, std::ios::cur);
    if (block_header.far_code_size > 0)
      file.seekg(block_header.far_code_size, std::ios::cur);

    // Validate the block against current RAM contents
    if (!ValidateBlock(block_header, original_instructions, system))
    {
      // Skip this block but continue reading
      continue;
    }

    validated_count++;

    // Recompile the block using the JIT. This emits fresh x64 code into
    // the current session's memory with valid absolute addresses.
    auto& ppc_state = system.GetPPCState();
    const CPUEmuFeatureFlags old_flags = ppc_state.feature_flags;
    ppc_state.feature_flags = static_cast<CPUEmuFeatureFlags>(block_header.feature_flags);

    // Check if block is already compiled (e.g., by HLE or prior JIT)
    if (!block_cache.GetBlockFromStartAddress(
            block_header.effective_address, ppc_state.feature_flags))
    {
      jit.Jit(block_header.effective_address);
      loaded_count++;
    }

    ppc_state.feature_flags = old_flags;
  }

  INFO_LOG_FMT(DYNA_REC,
               "AOT: Recompiled {}/{} blocks from {} ({} validated, {} failed validation)",
               loaded_count, file_header.block_count, path, validated_count,
               file_header.block_count - validated_count);

  return loaded_count;
}

bool AOTLoader::ValidateBlock(const AOTBlockHeader& header,
                              const std::vector<AOTOriginalInstruction>& original_instructions,
                              Core::System& system)
{
  auto& mmu = system.GetMMU();

  // Validate that the PPC instructions in RAM match what we compiled
  for (const auto& inst : original_instructions)
  {
    const auto translated = mmu.JitCache_TranslateAddress(inst.address);
    if (!translated.valid)
    {
      WARN_LOG_FMT(DYNA_REC, "AOT: Validate failed - cannot translate address {:08x}",
                   inst.address);
      return false;
    }

    const u32 current_inst = mmu.HostRead_Instruction(Core::CPUThreadGuard{system}, inst.address);
    if (current_inst != inst.instruction)
    {
      WARN_LOG_FMT(DYNA_REC,
                   "AOT: Validate failed - instruction mismatch at {:08x} "
                   "(expected {:08x}, got {:08x})",
                   inst.address, inst.instruction, current_inst);
      return false;
    }
  }

  // Validate static GQR assumptions
  auto& ppc_state = system.GetPPCState();
  if (header.static_gqr_mask != 0)
  {
    for (int gqr = 0; gqr < 8; ++gqr)
    {
      if ((header.static_gqr_mask & (1 << gqr)) != 0)
      {
        if (GQR(ppc_state, gqr) != header.static_gqr_values[gqr])
        {
          WARN_LOG_FMT(DYNA_REC, "AOT: Validate failed - GQR{} mismatch", gqr);
          return false;
        }
      }
    }
  }

  return true;
}

}  // namespace AOT64
