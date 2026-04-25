// Copyright 2024 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/PowerPC/AOT64/AOTFile.h"

#include <fmt/format.h>

#include "Common/CommonPaths.h"
#include "Common/FileUtil.h"

namespace AOT64
{

std::string BuildAOTCachePath(u32 rom_crc32, const std::string& game_id)
{
  std::string path = File::GetUserPath(D_CACHE_IDX);
  path += fmt::format("AOT/{}_{:08x}.aot", game_id, rom_crc32);
  return path;
}

}  // namespace AOT64
