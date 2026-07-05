// Copyright 2008 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// Detect the CPU, so we'll know which optimizations to use
#pragma once

#include <string>

enum class CPUVendor
{
  Intel,
  AMD,
  ARM,
  Other,
};

struct CPUInfo
{
  CPUVendor vendor = CPUVendor::Other;

  std::string cpu_id;
  std::string model_name;

  bool HTT = false;
  int num_cores = 0;

  bool bSSE3 = false;
  bool bSSSE3 = false;
  bool bSSE4_1 = false;
  bool bSSE4_2 = false;
  bool bLZCNT = false;
  bool bAVX = false;
  bool bAVX2 = false;
  bool bBMI1 = false;
  bool bBMI2 = false;
  // PDEP and PEXT are ridiculously slow on AMD Zen1, Zen1+ and Zen2 (Family 17h)
  bool bBMI2FastParallelBitOps = false;
  bool bFMA = false;
  bool bFMA4 = false;
  bool bAES = false;
  bool bMOVBE = false;
  // AVX-512 feature groups. Empty on parts that don't expose AVX-512F (x86-64-v4).
  // Detection requires CPUID leaf 7 sub-leaf 0 and an XGETBV result indicating that
  // the OS has enabled both ZMM_Hi256 + the opmask state (XCR0 bits 5,6,7).
  bool bAVX512F = false;
  bool bAVX512VL = false;
  bool bAVX512DQ = false;
  bool bAVX512BW = false;
  bool bAVX512CD = false;
  bool bAVX512VBMI2 = false;
  // This flag indicates that the hardware supports some mode
  // in which denormal inputs _and_ outputs are automatically set to (signed) zero.
  bool bFlushToZero = false;
  bool bAtom = false;
  bool bCRC32 = false;
  bool bSHA1 = false;
  bool bSHA2 = false;

  // ARMv8 specific
  bool bAFP = false;  // Alternate floating-point behavior

  // Call Detect()
  explicit CPUInfo();

  // The returned string consists of "<model_name>,<cpu_id>,<flag...>"
  // Where:
  //  model_name and cpud_id may be zero-length
  //  model_name is human-readable marketing name
  //  cpu_id is ':'-delimited string of id info
  //  flags are optionally included if the related feature is supported and reporting its enablement
  //  seems useful to report
  std::string Summarize();

private:
  void Detect();
};

extern CPUInfo cpu_info;
