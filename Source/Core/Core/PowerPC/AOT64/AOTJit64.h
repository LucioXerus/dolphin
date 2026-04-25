// Copyright 2024 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <memory>
#include <string>

#include "Core/PowerPC/Jit64/Jit.h"

namespace AOT64
{
class AOTLoader;
class AOTSerializer;
}  // namespace AOT64

// AOT-enabled JIT64 core.
// This behaves like JIT64 but can load pre-compiled blocks from an AOT cache
// and optionally serialize newly compiled blocks for future runs.
class AOTJit64 : public Jit64
{
public:
  explicit AOTJit64(Core::System& system);
  AOTJit64(const AOTJit64&) = delete;
  AOTJit64(AOTJit64&&) = delete;
  AOTJit64& operator=(const AOTJit64&) = delete;
  AOTJit64& operator=(AOTJit64&&) = delete;
  ~AOTJit64() override;

  void Init() override;
  void Shutdown() override;
  void ClearCache() override;

  // Override Jit to check AOT cache before compiling
  void Jit(u32 em_address) override;

  const char* GetName() const override { return "AOT64"; }

  // Set game identification for AOT cache file naming
  void SetGameInfo(u32 rom_crc32, const std::string& game_id);

  // Enable/disable AOT serialization (disabled by default)
  void SetSerializeEnabled(bool enabled) { m_serialize_enabled = enabled; }

  // Proactively compile all discoverable code in RAM before gameplay starts.
  // This populates the AOT cache so no JIT compilation is needed at runtime.
  // Returns number of blocks compiled.
  std::size_t PrecompileAll();

private:
  void PrecompileRange(u32 start, u32 end, CPUEmuFeatureFlags feature_flags);

  std::unique_ptr<AOT64::AOTLoader> m_loader;
  std::unique_ptr<AOT64::AOTSerializer> m_serializer;

  u32 m_rom_crc32 = 0;
  std::string m_game_id;
  bool m_serialize_enabled = false;
  bool m_cache_loaded = false;
  bool m_initialized = false;
  std::size_t m_blocks_serialized_this_session = 0;
  std::size_t m_next_progress_report = 50;
};
