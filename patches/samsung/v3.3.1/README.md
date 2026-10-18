# Samsung v3.3.1 patch set

Base: 0c0fdb960178fa12d80d6cbcfdcb6ad8be6a865d (official tag v3.3.1)
Version: 32680

Two patches, one responsibility each. They touch disjoint files, so the order
below is for review only. Indentation and naming follow upstream KernelSU.

1. KernelSU-v3.3.1-samsung-kdp-rkp-defex.patch
   Samsung kernel compatibility:
   - KDP credential install through prepare_ro_creds() / kdp_assign_pgd(), with the
     reference count taken and dropped through kdp_usecount_* so both ends of a
     credential's life use the same counter; DEFEX credential synchronization and
     the enforcement-point bypass, which is applied only to the task that loaded
     the module and to a root task already in the ksu domain; Kconfig/Kbuild wiring.
   - RKP-safe function replacement. On a target that refuses the slot write the
     write is not attempted at all: the function the slot points at gets one B at
     its entry, so the syscall table, the write_op array, sel_handle_status_ops.open
     and the LSM slots keep their value and every caller - the table included -
     still reaches the replacement. A 16-byte stub next to it keeps the displaced
     original callable (entry at +4: the original's kCFI type hash, a landing pad,
     the displaced instruction, B back), and the functions that reach it carry
     __nocfi, because a CFI-checked call to a page no module owns would panic.
     fn+0 is patched together with the branch in one aarch64_insn_patch_text()
     call, and a PACIASP at fn+0 becomes BTI C so the replacement does not sign an
     already-signed LR.
   - The route is the one diabl0w's KernelSU fork uses. Unlike his: branches are
     tracked and put back on module exit, a stub slot that gets reused is drained
     with synchronize_rcu_tasks() first, and the stub entry is published before the
     branch goes live, so a wrapper that saved its original can never recurse.
   - Callers that keep a saved original go through callthru_*: selinux_hide's four
     replacements (orig_* stays the real function for restoration), and the arm64
     syscall table hook, which also gains a callthru out-parameter and a read-back
     check that the dispatcher slot really holds the dispatcher.
   - ksu_put_cred() releases a credential through Samsung's own counter. Missing
     Samsung symbols degrade to the standard credential path instead of failing
     module init, so the same module still loads on non-Samsung targets.
   - No CONFIG_KSU_SAMSUNG_RKP / CONFIG_KSU_SAMSUNG_NO_PATCH_TEXT switch and no
     kprobe fallback: the route is compiled unconditionally and selected at runtime
     by the rkp_started symbol, and the files the kprobe route needed
     (hook/syscall_hook_manager.c, hook/x86_64/syscall_hook.c) are upstream again.

2. KernelSU-v3.3.1-samsung-lateload.patch
   ksud adaptation for the runtime (late-load) install path: stage_daemon_from()
   moves the helper's pre-staged copy into /data/adb/ksud with rename(), install()
   is split into stage_daemon()/finish_install() so the daemon is staged before
   loading the module changes this process's security context, the upstream
   manager restart is dropped, and the module is loaded with bundled=1 so the
   manager reports the kernel as its bundled LKM.

Installs that do not use the late-load flow only need patch 1.

Configuration:

- `CONFIG_KSU_SAMSUNG_KDP` enables the Samsung KDP credential path. A target
  without the helpers keeps the standard credential path instead of failing the
  module load.
- `CONFIG_KSU_SAMSUNG_DEFEX` enables the DEFEX credential synchronization and the
  enforcement-point bypass. It requires `CONFIG_KPROBES`.

Runtime contract for patch 2:
- the helper or the deployment script must place the loader at
  /data/local/tmp/.ksud-stage before late-load starts; the staged file has to be a
  root-owned regular file that group and others cannot write, and it is moved with
  rename(), so a missing or unsafe file fails staging and the previous daemon
  stays in place;
- the upstream manager restart (am force-stop / am start) no longer runs inside
  ksud, so the deployment has to (re)start the manager after late-load;
- bundled=1 makes the manager offer an embedded LKM update once its version code
  differs from the kernel's. Both sides are 32680, so it does not show, and a
  jailbreak install cannot follow that offer anyway.

Build flags:

CONFIG_KSU=m
CONFIG_KSU_SAMSUNG_KDP=y
CONFIG_KSU_SAMSUNG_DEFEX=y

Version 32680 is what Kbuild computes for this tree (30000 + git commit count).
The patch set overrides nothing, so a tree without .git reports 16.

Patch SHA256:

7E63326230212704F087DD21A60CE6875ED68365114CB6BB55C5AB9D9D67B259  compat
1AB8F75EC896BED91AB258A164D9E6B964A9B8F29A99EA0FEF788206F39D2D78  lateload

This branch is fork-local. Do not open a combined pull request to upstream.
