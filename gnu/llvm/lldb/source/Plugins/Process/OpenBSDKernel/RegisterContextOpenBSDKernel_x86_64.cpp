//===-- RegisterContextOpenBSDKernel_x86_64.cpp ---------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#if defined(__OpenBSD__)
#include <sys/types.h>
#include <sys/time.h>
#define _KERNEL
#include <machine/cpu.h>
#undef _KERNEL
#include <machine/pcb.h>
#endif

#include "RegisterContextOpenBSDKernel_x86_64.h"

#include "lldb/Target/Process.h"
#include "lldb/Target/Thread.h"
#include "lldb/Utility/RegisterValue.h"
#include "llvm/Support/Endian.h"

using namespace lldb;
using namespace lldb_private;

RegisterContextOpenBSDKernel_x86_64::RegisterContextOpenBSDKernel_x86_64(
    Thread &thread, RegisterInfoInterface *register_info, lldb::addr_t cpu_info)
    : RegisterContextPOSIX_x86(thread, 0, register_info), m_cpu_info(cpu_info) {
}

bool RegisterContextOpenBSDKernel_x86_64::ReadGPR() { return true; }

bool RegisterContextOpenBSDKernel_x86_64::ReadFPR() { return true; }

bool RegisterContextOpenBSDKernel_x86_64::WriteGPR() {
  assert(0);
  return false;
}

bool RegisterContextOpenBSDKernel_x86_64::WriteFPR() {
  assert(0);
  return false;
}

bool RegisterContextOpenBSDKernel_x86_64::ReadRegister(
    const RegisterInfo *reg_info, RegisterValue &value) {
  if (m_cpu_info == LLDB_INVALID_ADDRESS)
    return false;

  struct pcb pcb;
  Status error;
  struct cpu_info *ci = (struct cpu_info *)m_cpu_info;

  lldb::addr_t p_pcb =
    m_thread.GetProcess()->ReadPointerFromMemory((lldb::addr_t)&ci->ci_curpcb, error);

  size_t rd = m_thread.GetProcess()->ReadMemory(p_pcb, &pcb, sizeof(pcb), error);
  if (rd != sizeof(pcb))
    return false;

  uint32_t reg = reg_info->kinds[lldb::eRegisterKindLLDB];
  switch (reg) {
#define REG(x)                                                                 \
  case lldb_##x##_x86_64:                                                      \
    value = pcb.pcb_##x;                                                       \
    break;

    REG(rbp);
    REG(rsp);

#undef REG

  default:
    return false;
  }

  return true;
}

bool RegisterContextOpenBSDKernel_x86_64::WriteRegister(
    const RegisterInfo *reg_info, const RegisterValue &value) {
  return false;
}
