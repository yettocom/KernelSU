# Samsung patches for KernelSU

Upstream KernelSU has no Samsung support. On a Samsung GKI kernel `ksu_patch_text()`
fixmap writes are rejected by EL2 (RKP / proca), credential accounting sits behind
KDP write protection, and DEFEX treats the KSU domain as a privilege-escalation
target, so the stock module either fails to install its hooks or kills the `su`
process mid-`escape_to_root`.

This directory carries the patch set that makes KernelSU work on those kernels.

Only the newest set is kept: a new base replaces the previous one. History for each
file stays in `git log -- <path>`.

## Files

| Patch | Base | Size | Files | Content |
| --- | --- | --- | --- | --- |
| `KernelSU-v3.3.1-samsung-kdp-rkp-defex.patch` | KernelSU **v3.3.1** (tag `v3.3.1`) | 70,090 B | 15 | kernel side: RKP branch patcher, KDP credential commit, DEFEX synchronisation, LSM / syscall / selinux_hide wiring |
| `KernelSU-v3.3.1-samsung-lateload.patch` | same | 8,498 B | 2 | `userspace/ksud`: stage the daemon before loading the module in late-load mode |

The two patches touch disjoint files, so either can be applied on its own, and both
reverse cleanly with `git apply -R`.

## Apply

```sh
git -C <kernelsu-src> checkout v3.3.1
git -c core.autocrlf=false apply KernelSU-v3.3.1-samsung-kdp-rkp-defex.patch
git -c core.autocrlf=false apply KernelSU-v3.3.1-samsung-lateload.patch
```

## What the kernel patch does

- **RKP branch patcher** (`kernel/compat/samsung_rkp.c`): RKP blocks text writes, so
  the patcher branches a function entry to a 16-byte stub page and hands the caller
  a call-through pointer for the displaced instructions.
- **KDP** (`kernel/compat/samsung_kdp.c`): credential commits go through
  `prepare_ro_creds()` / `kdp_assign_pgd()`; the real bookkeeping runs under UMH,
  with a static work item and a `flush_work()` before returning. Missing symbols
  degrade to `kdp_available = false` instead of failing module init.
- **DEFEX** (`kernel/compat/samsung_defex.c`): kprobes on `task_defex_enforce` and
  `task_defex_user_exec` plus credential-triple synchronisation.
- **selinux_hide / syscall / LSM / policy hooks** are wired to the same
  call-through path so a hook is never live while its original is unavailable.

## Verify

```sh
git apply --check -R KernelSU-v3.3.1-samsung-*.patch   # against the patched tree
```
