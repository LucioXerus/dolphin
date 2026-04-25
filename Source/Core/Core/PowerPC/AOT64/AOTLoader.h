// Copyright 2024 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <memory>
#include <string>
#include <vector>

#include "Common/CommonTypes.h"
#include "Core/PowerPC/AOT64/AOTFile.h"

class Jit64;
class JitBlock;
class JitBlockCache;
class JitBaseBlockCache;

namespace Core
{
class System;
}  // namespace Core

namespace AOT64
{

// Loads AOT cache files and populates the JIT block cache.
class AOTLoader
{
public:
  AOTLoader();
  ~AOTLoader();

  // Attempt to load an AOT cache for the given game.
  // If successful, the blocks are inserted into the JIT block cache.
  // Returns number of blocks loaded, or 0 if no cache exists / invalid.
  std::size_t LoadCache(Jit64& jit, JitBaseBlockCache& block_cache, Core::System& system,
                        u32 rom_crc32, const std::string& game_id);

  // JitBlockCache variant for convenience
  std::size_t LoadCache(Jit64& jit, JitBlockCache& block_cache, Core::System& system,
                        u32 rom_crc32, const std::string& game_id);

  // Check if a valid AOT cache exists on disk
  bool CacheExists(u32 rom_crc32, const std::string& game_id) const;

  // Invalidate all loaded AOT blocks (e.g., on savestate load)
  void InvalidateAll(JitBaseBlockCache& block_cache);
  void InvalidateAll(JitBlockCache& block_cache);

private:
  bool ValidateBlock(const AOTBlockHeader& header,
                     const std::vector<AOTOriginalInstruction>& original_instructions,
                     Core::System& system);

  std::vector<JitBlock*> m_loaded_blocks;
};

}  // namespace AOT64
