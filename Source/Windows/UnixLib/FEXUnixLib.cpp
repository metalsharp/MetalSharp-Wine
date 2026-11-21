// SPDX-License-Identifier: MIT
#include <cstdint>
#include <type_traits>
#include <fcntl.h>
#include <string>
#include <sys/mman.h>
#ifdef __APPLE__
// macOS has no prctl: hardware TSO, kernel unaligned-atomic handling and VMA names are Linux
// interfaces, so those requests report failure and FEX keeps its own fallbacks.
static inline int prctl(int, ...) {
  return -1;
}
#else
#include <sys/prctl.h>
#endif
#include <unistd.h>
#ifdef __APPLE__
#include <cstring>
#include <dlfcn.h>
#include <mach-o/dyld.h>
#include <pthread.h>
#include <signal.h>
#include <sys/sysctl.h>
#endif

#include "FEXUnixLib.h"

// Equivalent to C++23's std::to_underlying.
template<typename Enum>
[[nodiscard]]
constexpr std::underlying_type_t<Enum> ToUnderlying(Enum e) noexcept {
  return static_cast<std::underlying_type_t<Enum>>(e);
}

using NTSTATUS = int32_t;
using unixlib_entry_t = NTSTATUS (*)(void*);

constexpr NTSTATUS STATUS_SUCCESS = 0;
constexpr NTSTATUS STATUS_NOT_SUPPORTED = 0xC00000BB;
constexpr NTSTATUS STATUS_INTERNAL_ERROR = 0xC00000E5;

#ifndef PR_GET_MEM_MODEL
#define PR_GET_MEM_MODEL 0x6d4d444c
#endif
#ifndef PR_SET_MEM_MODEL
#define PR_SET_MEM_MODEL 0x4d4d444c
#endif
#ifndef PR_SET_MEM_MODEL_DEFAULT
#define PR_SET_MEM_MODEL_DEFAULT 0
#endif
#ifndef PR_SET_MEM_MODEL_TSO
#define PR_SET_MEM_MODEL_TSO 1
#endif
#ifndef PR_ARM64_SET_UNALIGN_ATOMIC
#define PR_ARM64_SET_UNALIGN_ATOMIC 0x46455849
#endif
#ifndef PR_SET_VMA
#define PR_SET_VMA 0x53564d41
#endif
#ifndef PR_SET_VMA_ANON_NAME
#define PR_SET_VMA_ANON_NAME 0
#endif

static NTSTATUS FEXUnixLib_SetHardwareTSOControlHandler(void* _Args) {
  auto Args = reinterpret_cast<const FEXUnixLib_SetHardwareTSOControlArgs*>(_Args);
  if (Args->Enable) {
    int ret = prctl(PR_GET_MEM_MODEL, 0, 0, 0, 0);
    if (ret == PR_SET_MEM_MODEL_DEFAULT) {
      return prctl(PR_SET_MEM_MODEL, PR_SET_MEM_MODEL_TSO, 0, 0, 0) ? STATUS_NOT_SUPPORTED : STATUS_SUCCESS;
    }
    return ret == PR_SET_MEM_MODEL_TSO ? STATUS_SUCCESS : STATUS_NOT_SUPPORTED;
  }

  prctl(PR_SET_MEM_MODEL, PR_SET_MEM_MODEL_DEFAULT, 0, 0, 0);
  return STATUS_SUCCESS;
}

static NTSTATUS FEXUnixLib_SetKernelUnalignedAtomicControlHandler(void* _Args) {
  auto Args = reinterpret_cast<const FEXUnixLib_SetKernelUnalignedAtomicControl*>(_Args);
  return prctl(PR_ARM64_SET_UNALIGN_ATOMIC, Args->Flags, 0, 0, 0) == 0 ? STATUS_SUCCESS : STATUS_NOT_SUPPORTED;
}

static NTSTATUS FEXUnixLib_MadviseHandler(void* _Args) {
  auto Args = reinterpret_cast<const FEXUnixLib_Madvise*>(_Args);
#ifdef __APPLE__
  // FEX passes Linux advice values (MADV_HUGEPAGE/MADV_NOHUGEPAGE); they mean something else here.
  if (Args->Advise == 14 || Args->Advise == 15) {
    return STATUS_NOT_SUPPORTED;
  }
#endif
  return madvise(const_cast<void*>(Args->Addr), Args->Size, Args->Advise) == 0 ? STATUS_SUCCESS : STATUS_INTERNAL_ERROR;
}

static NTSTATUS FEXUnixLib_SetVMANameHandler(void* _Args) {
  auto Args = reinterpret_cast<const FEXUnixLib_SetVMAName*>(_Args);
  return prctl(PR_SET_VMA, PR_SET_VMA_ANON_NAME, reinterpret_cast<unsigned long>(Args->Addr), Args->Size, Args->Name) == 0 ?
           STATUS_SUCCESS :
           STATUS_INTERNAL_ERROR;
}

constexpr static int USER_PERMS = S_IRWXU | S_IRWXG | S_IRWXO;
static inline void* InitializeSHM(uint32_t MapSize, uint32_t MaxSize) {
  void* Base {};
  const std::string Name = "fex-" + std::to_string(getpid()) + "-stats";
  int fd = shm_open(Name.c_str(), O_CREAT | O_TRUNC | O_RDWR, USER_PERMS);

  if (fd == -1) {
    return nullptr;
  }

  if (ftruncate(fd, MapSize) == -1) {
    goto err;
  }

  // Reserve a region of MAX_STATS_SIZE so we can grow the allocation buffer.
  // Number of thread slots when ThreadStatsHeader == 64bytes and ThreadStats == 40bytes:
  // 1 page: 99 slots
  // 1 MB: 26211 slots
  // 128 MB: 3355440 slots
  Base = mmap(nullptr, MaxSize, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
  if (Base == MAP_FAILED) {
    Base = nullptr;
    goto err;
  }

  prctl(PR_SET_VMA, PR_SET_VMA_ANON_NAME, reinterpret_cast<void*>(Base), MaxSize, "FEXMem_Misc");

  // Allocate a small working shared space for now, grow as necessary.
  {
    auto SharedBase = mmap(Base, MapSize, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, fd, 0);
    if (SharedBase == MAP_FAILED) {
      ::munmap(Base, MaxSize);
      Base = nullptr;
      goto err;
    }
  }

err:
  close(fd);

  return Base;
}

static inline bool AllocateMoreSlots(void* SHMBase, uint32_t NewSize) {
  bool Result {};
  const std::string Name = "fex-" + std::to_string(getpid()) + "-stats";

  // When allocating more slots, open the fd without O_TRUNC | O_CREAT.
  int fd = shm_open(Name.c_str(), O_RDWR, USER_PERMS);
  if (fd == -1) {
    return false;
  }

  if (ftruncate(fd, NewSize) == -1) {
    goto err;
  }

  {
    auto SharedBase = mmap(SHMBase, NewSize, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, fd, 0);
    if (SharedBase == MAP_FAILED) {
      goto err;
    }
  }

  Result = true;

err:
  close(fd);
  return Result;
}

static NTSTATUS FEXUnixLib_GetSHMStatsVMAHandler(void* _Args) {
  auto Args = reinterpret_cast<FEXUnixLib_GetSHMStatsVMA*>(_Args);
  if (Args->SHMBase == nullptr) {
    Args->SHMBase = InitializeSHM(Args->MapSize, Args->MaxSize);
    return Args->SHMBase ? STATUS_SUCCESS : STATUS_INTERNAL_ERROR;
  }

  return AllocateMoreSlots(Args->SHMBase, Args->MapSize) ? STATUS_SUCCESS : STATUS_INTERNAL_ERROR;
}

static NTSTATUS FEXUnixLib_DeleteSHMStatsFileHandler(void* _Args) {
  const std::string Name = "fex-" + std::to_string(getpid()) + "-stats";
  shm_unlink(Name.c_str());
  return STATUS_SUCCESS;
}

static NTSTATUS FEXUnixLib_MapFileHandler(void* _Args) {
  auto Args = reinterpret_cast<FEXUnixLib_MapFile*>(_Args);
  // could find a way to dedupe with DiskCacheFilemapper in FEXInterpreter but it's barely any code
  // fd was already duped by Wine handle translation
  Args->Result = mmap(nullptr, Args->MapSize, PROT_READ, MAP_SHARED | MAP_NORESERVE, Args->FD, 0);
  close(Args->FD);
  if (Args->Result == (void*)-1) {
    Args->Result = nullptr;
    return STATUS_INTERNAL_ERROR;
  }
  return STATUS_SUCCESS;
}

static NTSTATUS FEXUnixLib_GetPIDHandler(void* _Args) {
  auto Args = reinterpret_cast<FEXUnixLib_GetPID*>(_Args);
  Args->Result = ::getpid();
  return STATUS_SUCCESS;
}
// MetalSharp: on macOS, asks Wine to back the JIT code buffers with MAP_JIT memory. From then on every write to them
// must be bracketed by JITWriteProtect(false/true) on the writing thread. Wine without the export keeps emulating W+X.
static NTSTATUS FEXUnixLib_EnableMapJitHandler(void*) {
#ifdef __APPLE__
  using EnableFn = int (*)();
  auto Enable = reinterpret_cast<EnableFn>(dlsym(RTLD_DEFAULT, "__wine_virtual_enable_map_jit"));
  // ntdll.so may be loaded without RTLD_GLOBAL: look it up among the loaded images.
  for (uint32_t i = 0, n = _dyld_image_count(); !Enable && i < n; i++) {
    const char* Name = _dyld_get_image_name(i);
    size_t Len = Name ? strlen(Name) : 0;
    if (Len < 9 || strcmp(Name + Len - 9, "/ntdll.so")) {
      continue;
    }
    if (void* Handle = dlopen(Name, RTLD_LAZY | RTLD_NOLOAD)) {
      Enable = reinterpret_cast<EnableFn>(dlsym(Handle, "__wine_virtual_enable_map_jit"));
      dlclose(Handle);
    }
  }
  return Enable && Enable() ? STATUS_SUCCESS : STATUS_NOT_SUPPORTED;
#else
  return STATUS_NOT_SUPPORTED;
#endif
}

static NTSTATUS FEXUnixLib_JITWriteProtectHandler(void* _Args) {
#ifdef __APPLE__
  pthread_jit_write_protect_np(reinterpret_cast<const FEXUnixLib_JITWriteProtect*>(_Args)->Enable);
  return STATUS_SUCCESS;
#else
  return STATUS_NOT_SUPPORTED;
#endif
}

#if defined(__APPLE__) && defined(__aarch64__)
namespace {
struct CPUIndexProbeThread {
  pthread_t Thread;
  uint32_t NumCPUs;
  uint32_t Matched;
  uint32_t Mismatched;
  // Processor numbers seen, modulo 64.
  uint64_t Seen;
  bool Error;
};

// Samples the register between two pthread_cpu_number_np calls; a sample counts only when both calls agree.
// That does not rule out a migration away and back in between, so a rare mismatch is tolerated (see below).
void* CPUIndexProbe(void* _Arg) {
  auto Arg = static_cast<CPUIndexProbeThread*>(_Arg);
  constexpr uint32_t Samples = 20000;
  for (uint32_t i = 0; i < Samples; ++i) {
    size_t Before {}, After {};
    uint64_t Reg {};
    if (pthread_cpu_number_np(&Before)) {
      Arg->Error = true;
      break;
    }
    __asm volatile("mrs %[Reg], TPIDR_EL0" : [Reg] "=r"(Reg));
    if (pthread_cpu_number_np(&After)) {
      Arg->Error = true;
      break;
    }
    if (Before != After) {
      continue;
    }
    ++Arg->Matched;
    // A migration can change the number between the reads, but never put it out of range.
    if (Before >= Arg->NumCPUs || (Reg & 0xfff) >= Arg->NumCPUs) {
      Arg->Error = true;
      break;
    }
    if ((Reg & 0xfff) != Before) {
      ++Arg->Mismatched;
    } else {
      Arg->Seen |= uint64_t {1} << (Before % 64);
    }
  }
  return nullptr;
}
} // namespace
#endif

// The macOS convention is not documented, so it is checked on threads of both performance classes, which the
// scheduler places on different core types, and fails closed. pthread_cpu_number_np reads the same register in
// macOS 27, so the comparison shows that libsystem still uses this layout and that the values are in range; it
// cannot see aliasing above 0xfff processors or a change that appears later in the process's life.
// A thread can migrate away and back between the reads (A-B-A); measured natively with this window, 0 in 80 million
// samples, so up to 1 mismatch in 1000 samples of each thread is taken as migration, while a wrong layout
// mismatches in most.
#if defined(__APPLE__) && defined(__aarch64__)
static bool ProbeCPUIndexInTPIDR(uint32_t NumCPUs) {
  int NCPU {};
  size_t Size = sizeof(NCPU);
  if (sysctlbyname("hw.ncpu", &NCPU, &Size, nullptr, 0) || Size != sizeof(NCPU) || NCPU <= 1 || NCPU > 0x1000 ||
      static_cast<uint32_t>(NCPU) != NumCPUs) {
    return false;
  }

  // Static, so that the threads' results outlive a failed join; the handler runs this at most once.
  constexpr uint32_t MaxThreads = 64;
  static CPUIndexProbeThread Threads[MaxThreads];
  const uint32_t NumThreads = NumCPUs * 2 < MaxThreads ? NumCPUs * 2 : MaxThreads;
  // The probe threads are not Wine threads: they must not take asynchronous signals meant for the process.
  sigset_t Block, Old;
  sigfillset(&Block);
  for (int Sig : {SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGTRAP}) {
    sigdelset(&Block, Sig);
  }
  if (pthread_sigmask(SIG_BLOCK, &Block, &Old)) {
    return false;
  }
  uint32_t Started = 0;
  bool Failed = false;
  for (; Started < NumThreads; ++Started) {
    auto& T = Threads[Started];
    T = {};
    T.NumCPUs = NumCPUs;
    pthread_attr_t Attr;
    if (pthread_attr_init(&Attr)) {
      Failed = true;
      break;
    }
    int Result = pthread_attr_set_qos_class_np(&Attr, Started % 2 ? QOS_CLASS_USER_INTERACTIVE : QOS_CLASS_BACKGROUND, 0);
    if (!Result) {
      Result = pthread_create(&T.Thread, &Attr, CPUIndexProbe, &T);
    }
    pthread_attr_destroy(&Attr);
    if (Result) {
      Failed = true;
      break;
    }
  }
  Failed |= pthread_sigmask(SIG_SETMASK, &Old, nullptr) != 0;

  uint64_t Matched = 0, Seen = 0;
  for (uint32_t i = 0; i < Started; ++i) {
    if (pthread_join(Threads[i].Thread, nullptr)) {
      Failed = true;
      continue;
    }
    const auto& T = Threads[i];
    Failed |= T.Error || uint64_t {T.Mismatched} * 1000 > T.Matched;
    Matched += T.Matched;
    Seen |= T.Seen;
  }
  // At least two processors seen, so the value is not a constant.
  return !Failed && Matched > 0 && __builtin_popcountll(Seen) >= 2;
}
#endif

static NTSTATUS FEXUnixLib_ProbeCPUIndexInTPIDRHandler(void* _Args) {
#if defined(__APPLE__) && defined(__aarch64__)
  // Once per process: FEX asks during host feature setup; a second call gets the first answer.
  static pthread_once_t Once = PTHREAD_ONCE_INIT;
  static uint32_t NumCPUs;
  static bool Works;
  NumCPUs = reinterpret_cast<const FEXUnixLib_ProbeCPUIndexInTPIDR*>(_Args)->NumCPUs;
  pthread_once(&Once, [] { Works = ProbeCPUIndexInTPIDR(NumCPUs); });
  return Works ? STATUS_SUCCESS : STATUS_NOT_SUPPORTED;
#else
  return STATUS_NOT_SUPPORTED;
#endif
}

extern "C" const unixlib_entry_t __wine_unix_call_funcs[] = {
  FEXUnixLib_SetHardwareTSOControlHandler,
  FEXUnixLib_SetKernelUnalignedAtomicControlHandler,
  FEXUnixLib_MadviseHandler,
  FEXUnixLib_SetVMANameHandler,
  FEXUnixLib_GetSHMStatsVMAHandler,
  FEXUnixLib_DeleteSHMStatsFileHandler,
  FEXUnixLib_MapFileHandler,
  FEXUnixLib_GetPIDHandler,
  FEXUnixLib_EnableMapJitHandler,
  FEXUnixLib_JITWriteProtectHandler,
  FEXUnixLib_ProbeCPUIndexInTPIDRHandler,
};

static_assert(sizeof(__wine_unix_call_funcs) / sizeof(__wine_unix_call_funcs[0]) == ToUnderlying(FEXUnixLibFunctions::MAX));
