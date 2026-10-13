# Samsung v3.3.1 patch set

Base: 0c0fdb960178fa12d80d6cbcfdcb6ad8be6a865d (official tag v3.3.1)
Version: 32680

Three patches, one responsibility each. They touch disjoint files, so the order
below is for review only. Indentation and naming follow upstream KernelSU.

1. KernelSU-v3.3.1-samsung-kdp-rkp-defex.patch
   Samsung kernel compatibility:
   - KDP credential install (prepare_ro_creds / kdp_assign_pgd) and DEFEX
     credential synchronization, plus the NO_PATCH_TEXT and Kconfig/Kbuild wiring
   - RKP-safe kprobe path that mirrors the syscall dispatcher set: execve,
     execveat, newfstatat, faccessat, statx and faccessat2, plus the setresuid
     kretprobe
   - a device without the Samsung KDP/DEFEX symbols now degrades to the standard
     credential path and logs why, instead of failing module init with -ENOENT
     (the v3.3.0 set refused to load on non-Samsung targets)
   - release version overrides (KSU_VERSION_CODE / KSU_VERSION_NAME)

2. KernelSU-v3.3.1-selinux_hide-kprobe.patch
   Kernel-side SELinux status hiding through kprobes (no kernel text patching);
   selected for Samsung targets through RKP or NO_PATCH_TEXT, and compiled only
   when CONFIG_KPROBES is on.
   Re-entry follows the KernelSU-Next samsung-lateload design: context and
   access divert only for uids in the app range, where the replacement never
   calls the probed original back, and status/setprocattr record the current
   task in a stack-allocated hashtable entry for the window in which they do.
   A pre-handler therefore allocates nothing and no entry outlives the call.
   A failed registration reports the underlying errno.

3. KernelSU-v3.3.1-samsung-lateload.patch
   ksud adaptation for the runtime (late-load) install path: stage_daemon_from()
   moves the helper's pre-staged copy into /data/adb/ksud with rename(), install()
   is split into stage_daemon()/finish_install(), and both daemonize() and the
   upstream manager restart are dropped.

Installs that do not use the late-load flow only need patches 1 and 2.

## Changes since v3.3.0

Baseline moved from v3.3.0 (932014ab) to v3.3.1 (0c0fdb96), 79 upstream commits.
Only three files really conflicted, each resolved by keeping both sides:

- kernel/core/init.c - upstream added the "package name" pr_info at the top of
  kernelsu_init(); the patch adds `int ret;` there. Both lines are kept.
- kernel/hook/syscall_hook_manager.c - upstream added the riscv64 syscall_regs
  include; the patch adds the Samsung kprobe block. Upstream include first.
- userspace/ksud/src/late_load.rs - upstream imports `Command` for the manager
  restart the patch removes, so the import list is just `std::time::Instant`.

The patch set also got smaller and closer to upstream:

- syscall-table hook reporting is done without changing the shared signature.
  `ksu_syscall_table_hook()` keeps upstream's `void` return (v3.3.1's riscv64
  implementation still returns void, so an `int` signature broke that build).
  Instead the helper records a hook only after the table write lands, warns on
  failure itself, and `ksu_syscall_hook_init()` verifies the dispatcher by
  reading the slot back. Two files left the patch set entirely:
  kernel/hook/syscall_hook.h and kernel/runtime/ksud_integration.c are now
  byte-identical to upstream.
- kernel/feature/sucompat.c is back to upstream (`void __exit
  ksu_sucompat_exit()` compiles cleanly for both LKM and built-in builds).
- kernel/Kbuild drops two redundant include flags and the stale version
  fallback is now 32680.
- kernel/compat/samsung_defex.c hoists its declarations (kernel C style).

Deliberately left as upstream, because it is upstream code and not a patch
regression: kernel/selinux/selinux.c escape_to_root_for_adb_root() installs
credentials with plain commit_creds() and does not run the KDP/DEFEX
synchronization. The DEFEX copy keeps the pre-transition (non-root) values
there, and task_defex_enforce() still clears the argument for a root task in the
u:r:ksu:s0 domain.

## Deployment note (late-load + manager)

In late-load mode the kernel skips the boot-completed manager scan
(`boot_complete skipped (late load)`) and only scans once, when the module is
loaded. Install the manager APK *before* `--late-load` (or reload the module
after installing it), otherwise the kernel never crowns the manager and the
manager UI keeps reporting "KernelSU is not installed" while `su` stays
unavailable for app uids.

Configuration:

- `CONFIG_KSU_SAMSUNG_KDP` enables the Samsung KDP credential path.
- `CONFIG_KSU_SAMSUNG_RKP` enables the RKP-safe kprobe paths. The sucompat
  syscall kprobes do not require `CONFIG_KRETPROBES`; the setresuid kretprobe is
  compiled only when `CONFIG_KRETPROBES` is available.
- `CONFIG_KSU_SAMSUNG_NO_PATCH_TEXT` can be enabled independently. It disables
  direct text patching and, together with `CONFIG_KSU_SAMSUNG_RKP`, selects the
  sucompat and selinux_hide kprobe fallbacks. `CONFIG_KPROBES` is required for
  those fallbacks; without it selinux_hide stays on the text route and fails
  with the real error instead of the `register_kprobe()` stub's `-ENOSYS`.

Runtime contract for patch 3:
- the helper or the deployment script must place the loader at
  /data/local/tmp/.ksud-stage before late-load starts; the staged copy is moved
  with rename() and there is no fallback, so a missing file fails staging;
- the upstream manager restart (am force-stop / am start) no longer runs inside
  ksud, so the deployment has to (re)start the manager after late-load.

Build flags:

CONFIG_KSU=m
CONFIG_KSU_SAMSUNG_KDP=y
CONFIG_KSU_SAMSUNG_RKP=y
CONFIG_KSU_SAMSUNG_DEFEX=y
CONFIG_KSU_SAMSUNG_NO_PATCH_TEXT=y

Version overrides for release builds (implemented by patch 1):

KSU_VERSION=32680
KSU_VERSION_CODE=32680
KSU_VERSION_NAME=3.3.1

Patch SHA256:

CE8D2906687BD673510356A136CFF2452B1BA8DA68EF665C832F6F31C806BDD0  compat
6818C929F6F56E178914426725ADE7059AD6C095208AFEC977CFC36DE2524A4B  selinux_hide
9EEB461560F9F14A9FDCBA460AFCB8BC2C658F66D99BF6A8A0115BFF03B3C21A  lateload

This branch is fork-local. Do not open a combined pull request to upstream.
