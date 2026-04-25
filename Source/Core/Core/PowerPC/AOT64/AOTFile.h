// Copyright 2024 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "Common/CommonTypes.h"
#include "Core/PowerPC/Gekko.h"

namespace PowerPC
{
class MMU;
}

namespace AOT64
{

// AOT cache file format version. Bump this when the format changes.
constexpr u32 AOT_FILE_VERSION = 1;

// Magic bytes for AOT cache files: "DOLAOT" followed by null
constexpr std::array<u8, 7> AOT_FILE_MAGIC = {'D', 'O', 'L', 'A', 'O', 'T', '\0'};

// Fixed virtual address where AOT code will be loaded.
// This must not conflict with other memory mappings.
// Chosen high in the user address space to avoid common heap/stack regions.
constexpr uintptr_t AOT_CODE_BASE_ADDRESS = 0x7F0000000000ULL;

#pragma pack(push, 1)

struct AOTFileHeader
{
  std::array<u8, 7> magic = AOT_FILE_MAGIC;
  u8 version = AOT_FILE_VERSION;
  u32 block_count = 0;
  u32 rom_crc32 = 0;
  // Total sizes for pre-allocation
  u32 total_near_code_size = 0;
  u32 total_far_code_size = 0;
  // Feature flags / config used at compile time
  u32 compile_time_feature_flags = 0;
  u8 compile_time_jit_options = 0;
  // Padding to 64 bytes
  u8 padding[35] = {};
};
static_assert(sizeof(AOTFileHeader) == 64, "AOTFileHeader must be 64 bytes");

struct AOTBlockHeader
{
  u32 effective_address = 0;
  u32 physical_address = 0;
  u32 feature_flags = 0;
  u32 original_size = 0;  // number of PPC instructions
  u32 near_code_size = 0;
  u32 far_code_size = 0;
  u32 num_link_data = 0;
  u32 num_physical_ranges = 0;
  // For validation: original PPC instructions
  u32 original_buffer_size = 0;  // in bytes = original_size * 8 (addr + inst)
  // Speculative optimization state
  u32 static_gqr_mask = 0;
  std::array<u32, 8> static_gqr_values = {};
  u32 speculative_constant_mask = 0;
  // Downcount for this block
  u32 downcount_amount = 0;
  // Padding to 96 bytes
  u8 padding[16] = {};
};
static_assert(sizeof(AOTBlockHeader) == 96, "AOTBlockHeader must be 96 bytes");

struct AOTLinkData
{
  u32 exit_address = 0;
  u8 call = 0;
  u8 padding[3] = {};
  // Offset within near code where the exit jump is located
  u32 exit_offset = 0;
};
static_assert(sizeof(AOTLinkData) == 12, "AOTLinkData must be 12 bytes");

struct AOTPhysicalRange
{
  u32 start = 0;
  u32 end = 0;
};
static_assert(sizeof(AOTPhysicalRange) == 8, "AOTPhysicalRange must be 8 bytes");

struct AOTOriginalInstruction
{
  u32 address = 0;
  u32 instruction = 0;
};
static_assert(sizeof(AOTOriginalInstruction) == 8, "AOTOriginalInstruction must be 8 bytes");

#pragma pack(pop)

// Represents a serialized block in memory before writing to disk
struct SerializedBlock
{
  AOTBlockHeader header;
  std::vector<AOTLinkData> link_data;
  std::vector<AOTPhysicalRange> physical_ranges;
  std::vector<AOTOriginalInstruction> original_instructions;
  std::vector<u8> near_code;
  std::vector<u8> far_code;
};

// Build a unique AOT cache path based on game ID and ROM hash
std::string BuildAOTCachePath(u32 rom_crc32, const std::string& game_id);

}  // namespace AOT64
