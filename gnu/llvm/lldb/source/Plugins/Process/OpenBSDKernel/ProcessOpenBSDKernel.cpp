//===-- ProcessOpenBSDKernel.cpp ------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "lldb/Core/Module.h"
#include "lldb/Core/PluginManager.h"
#include "lldb/Target/DynamicLoader.h"

#include "Plugins/DynamicLoader/Static/DynamicLoaderStatic.h"
#include "ProcessOpenBSDKernel.h"
#include "ThreadOpenBSDKernel.h"

#if defined(__OpenBSD__)
#include <kvm.h>
#endif

using namespace lldb;
using namespace lldb_private;

LLDB_PLUGIN_DEFINE(ProcessOpenBSDKernel)

namespace {

#if defined(__OpenBSD__)
class ProcessOpenBSDKernelKVM : public ProcessOpenBSDKernel {
public:
  ProcessOpenBSDKernelKVM(lldb::TargetSP target_sp, lldb::ListenerSP listener,
                          kvm_t *fvc);

  ~ProcessOpenBSDKernelKVM();

  size_t DoReadMemory(lldb::addr_t addr, void *buf, size_t size,
                      lldb_private::Status &error) override;

private:
  kvm_t *m_kvm;

  const char *GetError();
};
#endif // defined(__OpenBSD__)

} // namespace

ProcessOpenBSDKernel::ProcessOpenBSDKernel(lldb::TargetSP target_sp,
                                           ListenerSP listener_sp)
    : PostMortemProcess(target_sp, listener_sp) {}

lldb::ProcessSP ProcessOpenBSDKernel::CreateInstance(lldb::TargetSP target_sp,
                                                     ListenerSP listener_sp,
                                                     const FileSpec *crash_file,
                                                     bool can_connect) {
  ModuleSP executable = target_sp->GetExecutableModule();
  if (crash_file && !can_connect && executable) {
#if defined(__OpenBSD__)
    kvm_t *kvm =
        kvm_open(executable->GetFileSpec().GetPath().c_str(),
		 crash_file->GetPath().c_str(), nullptr, O_RDONLY, nullptr);
    if (kvm)
      return std::make_shared<ProcessOpenBSDKernelKVM>(target_sp, listener_sp,
                                                       kvm);
#endif
  }
  return nullptr;
}

void ProcessOpenBSDKernel::Initialize() {
  static llvm::once_flag g_once_flag;

  llvm::call_once(g_once_flag, []() {
    PluginManager::RegisterPlugin(GetPluginNameStatic(),
                                  GetPluginDescriptionStatic(), CreateInstance);
  });
}

void ProcessOpenBSDKernel::Terminate() {
  PluginManager::UnregisterPlugin(ProcessOpenBSDKernel::CreateInstance);
}

Status ProcessOpenBSDKernel::DoDestroy() { return Status(); }

bool ProcessOpenBSDKernel::CanDebug(lldb::TargetSP target_sp,
                                    bool plugin_specified_by_name) {
  return true;
}

void ProcessOpenBSDKernel::RefreshStateAfterStop() {}

bool ProcessOpenBSDKernel::DoUpdateThreadList(ThreadList &old_thread_list,
                                              ThreadList &new_thread_list) {
  if (old_thread_list.GetSize(false) == 0) {
    // Make up the thread the first time this is called so we can set our one
    // and only core thread state up.

    // We cannot construct a thread without a register context as that crashes
    // LLDB but we can construct a process without threads to provide minimal
    // memory reading support.
    switch (GetTarget().GetArchitecture().GetMachine()) {
    case llvm::Triple::aarch64:
    case llvm::Triple::x86:
    case llvm::Triple::x86_64:
      break;
    default:
      return false;
    }

    Status error;
    int32_t ncpu = ReadSignedIntegerFromMemory(
            FindSymbol("ncpu"), 4, -1, error);

    if (ncpu == -1)
        return false;

    lldb::addr_t cpu_info = FindSymbol("cpu_info");

    lldb::addr_t *ci =
      (lldb::addr_t *)ReadPointerFromMemory(cpu_info == 0 ?
					    FindSymbol("cpu_info_list") :
					    cpu_info, error);

    for (int i = 0; i < ncpu ; i++) {
        std::string thread_desc = llvm::formatv("cpu {0}", i);
        ThreadSP thread_sp{
            new ThreadOpenBSDKernel(*this, i, ci[i], thread_desc)};
        new_thread_list.AddThread(thread_sp);
    }
  }
  return new_thread_list.GetSize(false) > 0;
}

Status ProcessOpenBSDKernel::DoLoadCore() {
  // The core is already loaded by CreateInstance().
  return Status();
}

DynamicLoader *ProcessOpenBSDKernel::GetDynamicLoader() {
  if (m_dyld_up.get() == nullptr)
    m_dyld_up.reset(DynamicLoader::FindPlugin(
        this, DynamicLoaderStatic::GetPluginNameStatic()));
  return m_dyld_up.get();
}

lldb::addr_t ProcessOpenBSDKernel::FindSymbol(const char *name) {
  ModuleSP mod_sp = GetTarget().GetExecutableModule();
  const Symbol *sym = mod_sp->FindFirstSymbolWithNameAndType(ConstString(name));
  return sym ? sym->GetLoadAddress(&GetTarget()) : LLDB_INVALID_ADDRESS;
}

#if defined(__OpenBSD__)

ProcessOpenBSDKernelKVM::ProcessOpenBSDKernelKVM(lldb::TargetSP target_sp,
                                                 ListenerSP listener_sp,
                                                 kvm_t *fvc)
    : ProcessOpenBSDKernel(target_sp, listener_sp), m_kvm(fvc) {}

ProcessOpenBSDKernelKVM::~ProcessOpenBSDKernelKVM() {
  if (m_kvm)
    kvm_close(m_kvm);
}

size_t ProcessOpenBSDKernelKVM::DoReadMemory(lldb::addr_t addr, void *buf,
                                             size_t size, Status &error) {
  ssize_t rd = 0;
  rd = kvm_read(m_kvm, addr, buf, size);
  if (rd < 0 || static_cast<size_t>(rd) != size) {
    error.SetErrorStringWithFormat("Reading memory failed: %s", GetError());
    return rd > 0 ? rd : 0;
  }
  return rd;
}

const char *ProcessOpenBSDKernelKVM::GetError() { return kvm_geterr(m_kvm); }

#endif // defined(__OpenBSD__)
