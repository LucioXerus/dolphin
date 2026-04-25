// Copyright 2024 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <fstream>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "Common/CommonTypes.h"
#include "Core/PowerPC/AOT64/AOTFile.h"
#include "Core/PowerPC/PPCAnalyst.h"

class Jit64;
class JitBlock;
struct JitBlockData;

namespace PPCAnalyst
{
struct CodeBlock;
}

namespace AOT64
{

// Serializes JIT-compiled blocks to an AOT cache file.
class AOTSerializer
{
public:
  explicit AOTSerializer(u32 rom_crc32, const std::string& game_id);
  ~AOTSerializer();

  // Call once before adding blocks. Loads existing cache for merging.
  void Begin();

  // Add a compiled block to the serializer
  void AddBlock(const JitBlock& block, const PPCAnalyst::CodeBlock& code_block,
                const std::vector<std::pair<u32, u32>>& original_instructions, const u8* near_code,
                const u8* far_code, u32 downcount_amount, u32 static_gqr_mask,
                const std::array<u32, 8>& static_gqr_values);

  // Write all serialized blocks to disk
  bool Commit();

  bool IsOpen() const { return m_open; }
  std::size_t GetBlockCount() const { return m_blocks.size(); }

private:
  void LoadExistingBlocks();

  u32 m_rom_crc32;
  std::string m_game_id;
  std::vector<SerializedBlock> m_blocks;
  std::set<std::pair<u32, u32>> m_existing_blocks;  // (effective_address, feature_flags)
  bool m_open = false;
};

}  // namespace AOT64
