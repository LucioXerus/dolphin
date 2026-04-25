// Copyright 2024 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/PowerPC/AOT64/AOTSerializer.h"

#include <span>

#include "Common/FileUtil.h"
#include "Common/Logging/Log.h"
#include "Core/PowerPC/Jit64/Jit.h"
#include "Core/PowerPC/JitCommon/JitCache.h"

namespace AOT64
{

AOTSerializer::AOTSerializer(u32 rom_crc32, const std::string& game_id)
    : m_rom_crc32(rom_crc32), m_game_id(game_id)
{
}

AOTSerializer::~AOTSerializer() = default;

void AOTSerializer::Begin()
{
  // Load any existing blocks from disk so we can merge new ones.
  LoadExistingBlocks();
  m_open = true;
}

void AOTSerializer::LoadExistingBlocks()
{
  const std::string path = BuildAOTCachePath(m_rom_crc32, m_game_id);
  if (!File::Exists(path))
    return;

  std::ifstream file(path, std::ios::binary);
  if (!file)
    return;

  // Read file header
  AOTFileHeader file_header;
  file.read(reinterpret_cast<char*>(&file_header), sizeof(file_header));
  if (file.gcount() != sizeof(file_header))
    return;

  if (file_header.magic != AOT_FILE_MAGIC || file_header.version != AOT_FILE_VERSION)
    return;

  if (file_header.rom_crc32 != m_rom_crc32)
    return;

  // Read each block into memory
  for (u32 i = 0; i < file_header.block_count && file.good(); ++i)
  {
    SerializedBlock sb;

    file.read(reinterpret_cast<char*>(&sb.header), sizeof(sb.header));
    if (file.gcount() != sizeof(sb.header))
      break;

    if (sb.header.num_link_data > 0)
    {
      sb.link_data.resize(sb.header.num_link_data);
      file.read(reinterpret_cast<char*>(sb.link_data.data()),
                sb.link_data.size() * sizeof(AOTLinkData));
    }

    if (sb.header.num_physical_ranges > 0)
    {
      sb.physical_ranges.resize(sb.header.num_physical_ranges);
      file.read(reinterpret_cast<char*>(sb.physical_ranges.data()),
                sb.physical_ranges.size() * sizeof(AOTPhysicalRange));
    }

    if (sb.header.original_buffer_size > 0)
    {
      const std::size_t num_insts = sb.header.original_buffer_size / sizeof(AOTOriginalInstruction);
      sb.original_instructions.resize(num_insts);
      file.read(reinterpret_cast<char*>(sb.original_instructions.data()),
                sb.header.original_buffer_size);
    }

    if (sb.header.near_code_size > 0)
    {
      sb.near_code.resize(sb.header.near_code_size);
      file.read(reinterpret_cast<char*>(sb.near_code.data()), sb.header.near_code_size);
    }

    if (sb.header.far_code_size > 0)
    {
      sb.far_code.resize(sb.header.far_code_size);
      file.read(reinterpret_cast<char*>(sb.far_code.data()), sb.header.far_code_size);
    }

    // Deduplicate: skip if we already have this exact block
    const auto key = std::make_pair(sb.header.effective_address, sb.header.feature_flags);
    if (m_existing_blocks.insert(key).second)
    {
      m_blocks.push_back(std::move(sb));
    }
  }

  INFO_LOG_FMT(DYNA_REC, "AOT: Loaded {} existing blocks from cache for merge.", m_blocks.size());
}

void AOTSerializer::AddBlock(const JitBlock& block, const PPCAnalyst::CodeBlock& code_block,
                             const std::vector<std::pair<u32, u32>>& original_instructions,
                             const u8* near_code, const u8* far_code, u32 downcount_amount,
                             u32 static_gqr_mask, const std::array<u32, 8>& static_gqr_values)
{
  if (!m_open)
    return;

  // Deduplicate: don't add if already in cache
  const auto key = std::make_pair(block.effectiveAddress, block.feature_flags);
  if (!m_existing_blocks.insert(key).second)
    return;  // Already exists

  SerializedBlock sb;

  // Fill header
  sb.header.effective_address = block.effectiveAddress;
  sb.header.physical_address = block.physicalAddress;
  sb.header.feature_flags = block.feature_flags;
  sb.header.original_size = block.originalSize;
  sb.header.downcount_amount = downcount_amount;
  sb.header.static_gqr_mask = static_gqr_mask;
  sb.header.static_gqr_values = static_gqr_values;

  // Near code
  if (block.near_begin && block.near_end && block.near_end > block.near_begin)
  {
    sb.header.near_code_size = static_cast<u32>(block.near_end - block.near_begin);
    sb.near_code.resize(sb.header.near_code_size);
    std::memcpy(sb.near_code.data(), block.near_begin, sb.header.near_code_size);
  }

  // Far code
  if (block.far_begin && block.far_end && block.far_end > block.far_begin)
  {
    sb.header.far_code_size = static_cast<u32>(block.far_end - block.far_begin);
    sb.far_code.resize(sb.header.far_code_size);
    std::memcpy(sb.far_code.data(), block.far_begin, sb.header.far_code_size);
  }

  // Link data
  sb.header.num_link_data = static_cast<u32>(block.linkData.size());
  sb.link_data.reserve(block.linkData.size());
  for (const auto& link : block.linkData)
  {
    AOTLinkData ld;
    ld.exit_address = link.exitAddress;
    ld.call = link.call ? 1 : 0;
    ld.exit_offset =
        link.exitPtrs ? static_cast<u32>(link.exitPtrs - block.near_begin) : 0xFFFFFFFF;
    sb.link_data.push_back(ld);
  }

  // Physical ranges
  sb.header.num_physical_ranges = static_cast<u32>(block.physical_addresses.size());
  for (const auto& [start, end] : block.physical_addresses)
  {
    AOTPhysicalRange range;
    range.start = start;
    range.end = end;
    sb.physical_ranges.push_back(range);
  }

  // Original instructions for validation
  sb.header.original_buffer_size =
      static_cast<u32>(original_instructions.size() * sizeof(AOTOriginalInstruction));
  sb.original_instructions.reserve(original_instructions.size());
  for (const auto& [addr, inst] : original_instructions)
  {
    AOTOriginalInstruction aot_inst;
    aot_inst.address = addr;
    aot_inst.instruction = inst;
    sb.original_instructions.push_back(aot_inst);
  }

  m_blocks.push_back(std::move(sb));
}

bool AOTSerializer::Commit()
{
  if (!m_open || m_blocks.empty())
    return false;

  const std::string path = BuildAOTCachePath(m_rom_crc32, m_game_id);
  File::CreateFullPath(path);

  std::ofstream file(path, std::ios::binary);
  if (!file)
  {
    WARN_LOG_FMT(DYNA_REC, "AOT: Failed to open {} for writing", path);
    return false;
  }

  // Compute totals
  u32 total_near = 0;
  u32 total_far = 0;
  for (const auto& sb : m_blocks)
  {
    total_near += sb.header.near_code_size;
    total_far += sb.header.far_code_size;
  }

  // Write file header
  AOTFileHeader file_header;
  file_header.block_count = static_cast<u32>(m_blocks.size());
  file_header.rom_crc32 = m_rom_crc32;
  file_header.total_near_code_size = total_near;
  file_header.total_far_code_size = total_far;
  file.write(reinterpret_cast<const char*>(&file_header), sizeof(file_header));

  // Write each block
  for (const auto& sb : m_blocks)
  {
    file.write(reinterpret_cast<const char*>(&sb.header), sizeof(sb.header));

    if (!sb.link_data.empty())
      file.write(reinterpret_cast<const char*>(sb.link_data.data()),
                 sb.link_data.size() * sizeof(AOTLinkData));

    if (!sb.physical_ranges.empty())
      file.write(reinterpret_cast<const char*>(sb.physical_ranges.data()),
                 sb.physical_ranges.size() * sizeof(AOTPhysicalRange));

    if (!sb.original_instructions.empty())
      file.write(reinterpret_cast<const char*>(sb.original_instructions.data()),
                 sb.original_instructions.size() * sizeof(AOTOriginalInstruction));

    if (!sb.near_code.empty())
      file.write(reinterpret_cast<const char*>(sb.near_code.data()), sb.near_code.size());

    if (!sb.far_code.empty())
      file.write(reinterpret_cast<const char*>(sb.far_code.data()), sb.far_code.size());
  }

  m_open = false;
  INFO_LOG_FMT(DYNA_REC, "AOT: Wrote {} blocks to {}", m_blocks.size(), path);
  return true;
}

}  // namespace AOT64
