# Samsung v3.3.0 patch set

Base: 932014ab5b2c9b74a3d11e2ec4d17dd10fc9442e (official tag v3.3.0)
Version: 32601

Three patches, one responsibility each. They touch disjoint files, so the order
below is for review only. Indentation and naming follow upstream KernelSU.

1. KernelSU-v3.3.0-samsung-kdp-rkp-defex.patch
   Samsung kernel compatibility:
   - KDP credential install (prepare_ro_creds / kdp_assign_pgd) and DEFEX
     credential synchronization, plus the NO_PATCH_TEXT and Kconfig/Kbuild wiring
   - RKP-safe kprobe path that mirrors the syscall dispatcher set: execve,
     execveat, newfstatat, faccessat, statx and faccessat2, plus the setresuid
     kretprobe
   - ksu_syscall_table_hook() reports failures on arm64 and x86_64 alike, and the
     ksud integration logs when the __NR_read/__NR_fstat hooks cannot be installed
   - release version overrides (KSU_VERSION_CODE / KSU_VERSION_NAME)

2. KernelSU-v3.3.0-selinux_hide-kprobe.patch
   Kernel-side SELinux status hiding through kprobes (no kernel text patching);
   selected for Samsung targets through RKP or NO_PATCH_TEXT, and compiled only
   when CONFIG_KPROBES is on.
   Re-entry follows the KernelSU-Next samsung-lateload design: context and
   access divert only for uids in the app range, where the replacement never
   calls the probed original back, and status/setprocattr record the current
   task in a stack-allocated hashtable entry for the window in which they do.
   A pre-handler therefore allocates nothing and no entry outlives the call.
   A failed registration reports the underlying errno.

3. KernelSU-v3.3.0-samsung-lateload.patch
   ksud adaptation for the runtime (late-load) install path: stage_daemon_from()
   moves the helper's pre-staged copy into /data/adb/ksud with rename(), install()
   is split into stage_daemon()/finish_install(), and both daemonize() and the
   upstream manager restart are dropped.

Installs that do not use the late-load flow only need patches 1 and 2.

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

KSU_VERSION=32601
KSU_VERSION_CODE=32601
KSU_VERSION_NAME=3.3.0

Patch SHA256:

D1FD7B1664BA471346B4A3ECD0C1F0608FFAAC62A0153D57DBC7164C11B0E29F  compat
E60F41EF093E275569B1D64382B0F0CAAB0718EF85C83756B101693D81DFDFEC  selinux_hide
A388942853797D22635B5F94D35B323CF0DEB54AFD82F745FA36A7CAF9B8D936  lateload

This branch is fork-local. Do not open a combined pull request to upstream.
