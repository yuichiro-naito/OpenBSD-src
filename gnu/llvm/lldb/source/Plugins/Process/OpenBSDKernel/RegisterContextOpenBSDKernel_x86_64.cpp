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
#include <frame.h>
#endif

#include "RegisterContextOpenBSDKernel_x86_64.h"

#include "lldb/Target/Process.h"
#include "lldb/Target/Thread.h"
#include "lldb/Utility/RegisterValue.h"
#include "llvm/Support/Endian.h"

using namespace lldb;
using namespace lldb_private;

RegisterContextOpenBSDKernel_x86_64::RegisterContextOpenBSDKernel_x86_64(
    Thread &thread, RegisterInfoInterface *register_info,
    lldb::addr_t cpu_info, lldb::addr_t dumppcb)
  : RegisterContextPOSIX_x86(thread, 0, register_info), m_cpu_info(cpu_info),
    m_dumppcb(dumppcb) {
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
  struct switchframe sf;
  Status error;
  struct cpu_info *ci = (struct cpu_info *)
    m_thread.GetProcess()->ReadPointerFromMemory(m_cpu_info, error);

  lldb::addr_t p_pcb =
    m_thread.GetProcess()->ReadPointerFromMemory((lldb::addr_t)&ci->ci_curpcb, error);

  size_t rd = m_thread.GetProcess()->ReadMemory(p_pcb, &pcb, sizeof(pcb), error);
  if (rd != sizeof(pcb))
    return false;

  if (pcb.pcb_rbp == 0) {
    rd = m_thread.GetProcess()->ReadMemory(m_dumppcb, &pcb, sizeof(pcb), error);
    if (rd != sizeof(pcb))
      return false;
  }

  rd = m_thread.GetProcess()->ReadMemory(pcb.pcb_rbp, &sf, sizeof(sf), error);
  if (rd != sizeof(sf))
    return false;

  bool use_switchframe = ((u_int64_t)sf.sf_rbp == pcb.pcb_rbp);

  uint32_t reg = reg_info->kinds[lldb::eRegisterKindLLDB];
  switch (reg) {
  case lldb_rbp_x86_64:
    value = pcb.pcb_rbp;
    break;
  case lldb_rsp_x86_64:
    value = (use_switchframe) ? pcb.pcb_rsp : pcb.pcb_rsp + 8;
    break;
  case lldb_rip_x86_64:
    value = (use_switchframe) ? (u_int64_t)sf.sf_rip : *((u_int64_t*)&sf);
    break;

  default:
    return false;
  }

  return true;
}

bool RegisterContextOpenBSDKernel_x86_64::WriteRegister(
    const RegisterInfo *reg_info, const RegisterValue &value) {
  return false;
}
