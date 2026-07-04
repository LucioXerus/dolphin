// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/PowerPC/JitAot64/AotCache.h"

#include <array>
#include <string>
#include <vector>

#include "Common/CommonPaths.h"
#include "Common/CommonTypes.h"
#include "Common/Config/Config.h"
#include "Common/FileUtil.h"
#include "Common/IOFile.h"
#include "Common/Logging/Log.h"

#include "Core/Config/MainSettings.h"
#include "Core/ConfigManager.h"

namespace AotJit
{
namespace
{
// "AOTC" in little endian.
constexpr u32 CACHE_MAGIC = 0x43544F41;
// Bump whenever the on-disk layout or the meaning of an entry changes.
constexpr u32 CACHE_VERSION = 1;

struct CacheHeader
{
  u32 magic;
  u32 version;
  u32 settings_hash;
  u32 entry_count;
};

void HashFold(u32* hash, u32 value)
{
  // FNV-1a over the four bytes of `value`.
  for (int i = 0; i < 4; ++i)
  {
    *hash ^= (value >> (i * 8)) & 0xff;
    *hash *= 0x01000193;
  }
}
}  // namespace

u32 AotCache::ComputeSettingsHash()
{
  // Every setting listed here changes the machine code the JIT emits for a given block,
  // so a cache produced with different values must not be reused.
  u32 hash = 0x811c9dc5;
  HashFold(&hash, CACHE_VERSION);
  HashFold(&hash, Config::Get(Config::MAIN_FASTMEM) ? 1 : 0);
  HashFold(&hash, Config::Get(Config::MAIN_PAGE_TABLE_FASTMEM) ? 1 : 0);
  HashFold(&hash, Config::Get(Config::MAIN_FASTMEM_ARENA) ? 1 : 0);
  HashFold(&hash, Config::Get(Config::MAIN_ACCURATE_CPU_CACHE) ? 1 : 0);
  HashFold(&hash, Config::Get(Config::MAIN_MMU) ? 1 : 0);
  HashFold(&hash, Config::Get(Config::MAIN_FLOAT_EXCEPTIONS) ? 1 : 0);
  HashFold(&hash, Config::Get(Config::MAIN_DIVIDE_BY_ZERO_EXCEPTIONS) ? 1 : 0);
  HashFold(&hash, Config::Get(Config::MAIN_LOW_DCBZ_HACK) ? 1 : 0);
  HashFold(&hash, Config::Get(Config::MAIN_FPRF) ? 1 : 0);
  HashFold(&hash, Config::Get(Config::MAIN_ACCURATE_NANS) ? 1 : 0);
  HashFold(&hash, Config::Get(Config::MAIN_ACCURATE_FMADDS) ? 1 : 0);
  HashFold(&hash, Config::Get(Config::MAIN_JIT_FOLLOW_BRANCH) ? 1 : 0);
  HashFold(&hash, Config::Get(Config::MAIN_ENABLE_DEBUGGING) ? 1 : 0);
  return hash;
}

std::string AotCache::GetCacheFilePath(const Core::System& system)
{
  const std::string game_id = SConfig::GetInstance().GetGameID();
  if (game_id.empty())
    return {};

  const std::string dir = File::GetUserPath(D_CACHE_IDX) + "AOT" DIR_SEP;
  if (!File::IsDirectory(dir))
    File::CreateFullPath(dir);

  return fmt::format("{}{}-r{}.aotcache", dir, game_id, SConfig::GetInstance().GetRevision());
}

bool AotCache::Load(const std::string& path, u32 expected_settings_hash,
                    std::vector<AotEntry>* out_entries)
{
  out_entries->clear();

  if (path.empty() || !File::Exists(path))
    return false;

  File::IOFile file(path, "rb");
  if (!file)
    return false;

  CacheHeader header{};
  if (!file.ReadArray(&header, 1))
    return false;

  if (header.magic != CACHE_MAGIC || header.version != CACHE_VERSION)
  {
    WARN_LOG_FMT(DYNA_REC, "AOT cache '{}' has an incompatible format; ignoring.", path);
    return false;
  }

  if (header.settings_hash != expected_settings_hash)
  {
    INFO_LOG_FMT(DYNA_REC, "AOT cache '{}' was built with different settings; ignoring.", path);
    return false;
  }

  // Guard against a corrupt count claiming a huge allocation. The 4 GiB / 4-byte address
  // space bounds the number of distinct block entry points (times the 8 feature-flag combos).
  constexpr u32 MAX_ENTRIES = (0x1'0000'0000ull / 4) & 0xffff'ffff;
  if (header.entry_count > MAX_ENTRIES)
  {
    WARN_LOG_FMT(DYNA_REC, "AOT cache '{}' has an implausible entry count; ignoring.", path);
    return false;
  }

  out_entries->resize(header.entry_count);
  if (header.entry_count != 0 && !file.ReadArray(out_entries->data(), header.entry_count))
  {
    WARN_LOG_FMT(DYNA_REC, "AOT cache '{}' is truncated; ignoring.", path);
    out_entries->clear();
    return false;
  }

  INFO_LOG_FMT(DYNA_REC, "Loaded {} AOT entries from '{}'.", out_entries->size(), path);
  return true;
}

bool AotCache::Save(const std::string& path, u32 settings_hash,
                    const std::vector<AotEntry>& entries)
{
  if (path.empty())
    return false;

  const std::string temp_path = path + ".tmp";
  {
    File::IOFile file(temp_path, "wb");
    if (!file)
    {
      WARN_LOG_FMT(DYNA_REC, "Failed to open AOT cache '{}' for writing.", temp_path);
      return false;
    }

    const CacheHeader header{CACHE_MAGIC, CACHE_VERSION, settings_hash,
                             static_cast<u32>(entries.size())};
    if (!file.WriteArray(&header, 1) ||
        (!entries.empty() && !file.WriteArray(entries.data(), entries.size())))
    {
      WARN_LOG_FMT(DYNA_REC, "Failed to write AOT cache '{}'.", temp_path);
      return false;
    }
  }

  if (!File::Rename(temp_path, path))
  {
    WARN_LOG_FMT(DYNA_REC, "Failed to move AOT cache into place at '{}'.", path);
    File::Delete(temp_path);
    return false;
  }

  INFO_LOG_FMT(DYNA_REC, "Saved {} AOT entries to '{}'.", entries.size(), path);
  return true;
}
}  // namespace AotJit
