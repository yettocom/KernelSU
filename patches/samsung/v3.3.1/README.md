# KernelSU 三星适配补丁（基线 `v3.3.1`）

基线 `v3.3.1`（tag `0c0fdb96`，APK code `32680`）+ 下面两份补丁 = 本目录的成品。
工作树 `D:\root\ksu-v331-work`（游离 HEAD = v3.3.1，补丁不提交）。

| 补丁 | 字节 | 覆盖 |
|---|---|---|
| `KernelSU-v3.3.1-samsung-kdp-rkp-defex.patch` | 70,090 | `Kbuild`/`Kconfig`、`compat/samsung_{kdp,rkp,defex}.c`(+`.h`)、`include/ksu_samsung_{kdp,rkp}.h`、`core/init.c`、`feature/selinux_hide.c`、`hook/arm64/{patch_memory,syscall_hook}.c`、`hook/tp_marker.c`、`policy/app_profile.c`、`runtime/ksud_integration.c`（15 文件，+1246/-29） |
| `KernelSU-v3.3.1-samsung-lateload.patch` | 8,157 | `userspace/ksud/src/{late_load.rs,utils.rs}`（2 文件，+67/-24） |

合计 **17 文件 / +1516 · -55**。补丁之外的文件逐字节等于 `v3.3.1`：`feature/sucompat.c`、
`hook/syscall_hook.h`、`hook/{syscall_hook_manager.c,x86_64/syscall_hook.c,riscv64/syscall_hook.c}`、
`manager/build.gradle.kts`、`userspace/ksud/build.rs`。

> 生成补丁要用 `git -c core.autocrlf=true diff <tag> -- <paths>`：工作树是 Windows 检出（全树 CRLF），
> tag 里的 blob 是 LF，不加这个开关会把 670 个文件都算成改动（110k+ 行噪声）。新增文件要先
> `git add -N`，否则 `git diff` 看不见它们。

## 路线

三星固件的 RKP 在 EL2 保护内核 text 与 rodata，`ksu_patch_text()` 那套 fixmap 直写会被拒。
所以**不写槽位，改写函数**：把槽位指向的那个函数的入口换成一条 `B`，槽位保持原值——表项、
间接调用、任何调用方都照样走到替换函数。

1. `hook/arm64/patch_memory.c`（+10）在 `ksu_patch_text()` 最前面加一层转发：
   `len == sizeof(void *) && ksu_samsung_rkp_active()` → 走分支路线；其余仍走上游的
   `phys_from_virt()` + fixmap + `stop_machine`。非 RKP 设备行为一个字节都不变。
2. `compat/samsung_rkp.c`（551 行）在函数入口写 `B`，并把被顶掉的那条指令做成 16 字节桩：

   | 桩偏移 | 内容 |
   |---|---|
   | +0 | 原函数 `fn-4` 那个字（kCFI type hash，`__cfi_*` 就落在那里） |
   | +4 | landing pad：原 fn+0 的 PAC 指令，或 `BTI C` |
   | +8 | 被分支顶掉的那条指令 |
   | +12 | `B` 回原函数里被顶指令的下一条 |

   `ksu_samsung_rkp_get_stub()` 交给调用方的就是 `桩+4`。写桩与写函数入口都走
   `aarch64_insn_patch_text()`——kprobe 用的同一条路径。
3. 桩页是 `module_alloc`/`execmem_alloc` 出来的裸页，不在任何 module 的映射里，**只能用普通调用进**：
   CFI 检查会打到 `__cfi_slowpath_diag`。所以进桩的函数都标 `__nocfi`（selinux_hide 的四个
   `my_*`、`ksu_sys_read`/`ksu_sys_fstat`、`ksu_samsung_defex_sync_current`、`ksu_samsung_kdp_put_cred`）。
4. 调用方保存的"原函数"此时指向分支，不能再直接调，于是把"真值"和"调用入口"拆开：
   `feature/selinux_hide.c`（+57/-8）用 `orig_*` 留真值（还原时写回槽位）、`callthru_*` 才是
   真正调用的入口；`hook/arm64/syscall_hook.c`（+39/-5）给 `patch_syscall_table()` 多一个
   `callthru` 出参，`ksu_syscall_table_hook(nr, fn, old)` 把 `old` 交下去，于是
   `orig_sys_read`/`orig_sys_fstat`（`runtime/ksud_integration.c`，+15/-2）直接拿到桩。
5. 发布顺序是**先填桩 → 再把桩入口写给调用方 → 最后写 `B`**（`..._branch_patch_callthru()`）。
   反过来不是窗口而是**永久递归**：`B` 先生效而调用方还拿着真原函数，而真原函数的入口已经是 `B`。
6. 还原走同一个函数：分支路线下槽位从没被写过，所以还原时读到的 `*(void **)dst` 仍是原函数，
   命中站点表 → 写回被顶的两条指令 → `new_fn == fn` 早退，不会自环。模块卸载时
   `ksu_samsung_rkp_exit()` 逆序还原全部站点，**全部成功**才释放桩页。
7. 站点上限 16 个、桩每页 256 个。还原交回的槽位进空闲表，**复用一个槽之前先
   `synchronize_rcu_tasks()` 排空**还在旧桩里的调用；解析不到这个符号就不回收（退回旧行为）。

必须做对的几个细节：

- fn+0 是 `PACIASP/PACIBSP` 时不能留在原地：替换函数自己也会签一次，LR 被签两次，尾声 `AUT`
  失败 → FPAC 致命异常。做法是把 fn+0 换成 `BTI C`（合法 landing pad、不签名），原 PAC 指令
  搬进桩里替原函数签。PAC 只允许在 **fn+0** 被跳过：`bti c; paciasp` 这种布局里 `paciasp` 必须
  自己成为被搬进桩的那条，跳到 fn+8 就是 LR 从未被签。
- fn+0 与入口那条 `B` 合成**一次** `aarch64_insn_patch_text(addrs, insns, 2)`，还原对称，
  不让任何 CPU 看到"fn+0 已还原、分支还在"的半成品。
- 搬进桩的指令不能是 PC 相对的（`ADR/ADRP`、`LDR literal`、`B/BL`、`B.cond`、`CBZ/CBNZ`、
  `TBZ/TBNZ`），否则它算的是桩的地址；命中返回 `-EINVAL`，不装。
- `B` 只有 ±128 MB；够不着返回 `-ERANGE`，只记日志、不装。
- 桩页用 `set_memory_rox()`（6.3+）或 `set_memory_ro()+set_memory_x()` 锁成不可写，返回值
  会检查，失败就释放该页返回错误——不把可写页当代码用。

## 为什么是这条路线

**为什么不直写槽位**：RKP 把 text/rodata 从内核手里拿走，fixmap 写会被拒。这是整套补丁的前提。

**为什么不留 kprobe 兜底、不留 `CONFIG_KSU_SAMSUNG_RKP` / `NO_PATCH_TEXT` 开关**：分支路线覆盖了
原来靠 kprobe 的三块（`selinux_hide.c` 的四个槽、`syscall_hook_manager.c` 的 sucompat 兜底、
`x86_64/syscall_hook.c` 的对称改动），留着就是两套实现同时维护；开关本身也没有第二个取值
（分支路线无条件编译，运行时由 `rkp_started` 决定）。

**为什么"未实现 syscall 也进 dispatcher"不是问题**：分支补丁写的是**共享的**
`__arm64_sys_ni_syscall` 入口，所有未实现 syscall 的槽位都指向它，于是它们也会进 dispatcher。
`ksu_syscall_dispatcher()` 第一句就是 `if (regs->syscallno != ksu_dispatcher_nr) return -ENOSYS;`，
正好等于 `ni_syscall` 原本的返回值，行为不变。

**为什么 DEFEX 用"重定向到返回 0 的函数"而不是清参数**：清 `task_defex_enforce` 的第一个参数，
只在三星那个函数恰好检查 NULL 时成立；跳到一个"回答无违规"的函数不依赖这个假设，也是社区那份
验证过的做法。放行范围只限**加载者本身**和**已经在 ksu 域的 root 任务**，不是全局关。

**为什么 KDP 要自己装 cred**：新固件上普通 `commit_creds()` 这条路走不通，得用三星的
`prepare_ro_creds()` / `kdp_assign_pgd()`，引用计数也走 `kdp_usecount_*`（见"依据"）。
缺符号时**降级成标准路径而不是让 init 失败**（`kdp_available` / `defex_available` 守卫），
这样同一份 ko 在非三星设备上也装得上。

**late-load 为什么传 `bundled=1`**：`ksu_bundled` 是内核本来就有的模块参数
（`module_param_named(bundled, ksu_bundled, bool, 0)`，默认 false），`do_get_info()` 只在它为真时置
`KSU_GET_INFO_FLAG_BUNDLED`，管理器据此给 KernelSU 那栏挂"自定义 LKM"标签。上游 late-load 不传它
（注释说 jailbreak 模式刷不了 boot），而我们的管理器是官方 APK、ko 是自己编的，标签本该出现——
这里按需要去掉，做法只是那两个分支各加 `bundled=1`，没动内核默认值，普通 `insmod` 仍如实报告。
副作用：`isLkmBundled` 为真后，管理器版本号 ≠ 内核版本号时会提示"有可用的内嵌 LKM 更新"；
本包两边都是 32680，不会出现。

## 依据

### 真机符号与反汇编（S23U chc/tgy、S24U）

| 假设 | 证据 |
|---|---|
| 这台机器上 RKP 确实开着 | kallsyms 里有 `rkp_started`，`aarch64_insn_patch_text` 也在 |
| `task_defex_user_exec` 是空函数，不该给它挂 kprobe | 反汇编 `paciasp; mov w0, wzr; autiasp; ret` |
| `task_defex_enforce` 清掉第一个参数会提前返回 0 | 入口 `cbz x0, #84`，落点就是 `mov w0, w22`（w22 初值 0）的返回路径 |
| fn+0 是 `PACIASP` 时换 `BTI C`、分支放 fn+4 | `__arm64_sys_read` = `paciasp; stp x29,x30,[sp,#-16]!; …`；`__arm64_sys_ni_syscall` = `paciasp; mov x0,#-38; autiasp; ret`（不是 PC 相对指令，能搬进桩） |
| 替换函数调桩是普通 `blr`，不会二次签名 | ko 里 `my_write_context`/`my_setprocattr`/`ksu_sys_read` 都是 `ldr x8,[…]; blr x8` |
| `__nocfi` 真的去掉了 kCFI 检查 | 加之前 `blr` 前有 `sub/ror/cmp/b.hs`，加之后只剩 `ldr` |
| `aarch64_insn_patch_text(addrs, insns, cnt)` 的签名 | 入口 `sub sp,#48 … cmp w2,#1 … b.lt`，x0/x1/w2 对得上 |
| `B` 够得着 | `_stext`=0xffffffc008140000、ko text=0xffffffc003e37fc4，距离 67 MB |
| `kdp_usecount_dec_and_test()` 对**所有** cred 都成立 | 真机 `struct cred` 是 176 B 的标准 5.15 布局（没有额外 KDP 字段），计数在受保护区域里按地址算 |

### 内核源码（v5.15 原文）与上游出处

- `commit_creds()`：发布指针前跑 `set_dumpable()` / `pdeath_signal` / `smp_wmb` 与
  `key_fsuid_changed()` / `key_fsgid_changed()`，并给新 cred 取**第二引用**（注释
  "we will require a ref for the subj creds too"）；`exit_creds()` 对两个槽各 `put` 一次。
  KDP 那条路径逐项照抄，第二引用走 `kdp_set_cred_non_rcu(ro_cred, 0)` + `kdp_usecount_inc(ro_cred)`
  （`get_cred()` 的两步，顺序一致）；失败时 cred 留给调用方的 `abort_creds()`。
- `__arm64_sys_ni_syscall` 返回 `-ENOSYS`：dispatcher 的守卫就是照它写的。
- 上游 v3.3.1 的 `ksu_patch_text()`（fixmap + `stop_machine`）、`ksu_syscall_table_hook()` 的
  `void` 签名、`late_load::run()` 走 `utils::install()`（`remove_file` + `copy`，而且在
  `load_module` 之后才做）——补丁只在必要处动它们。
- 模块导入合法性：三个 KMI 的 modpost 日志里没有 `undefined`。

### 社区参照

路线取自 diabl0w 的 KernelSU fork（`ksu_rkp_branch_patch()` 写函数入口、DEFEX 重定向）。
与他不同的地方：

| | diabl0w | 这里 |
|---|---|---|
| 还原 | 只装不撤 | 站点表 + 卸载时逆序还原，全成功才放桩页 |
| 桩槽位 | 只增不减 | 空闲表 + 复用前 `synchronize_rcu_tasks()` 排空 |
| 桩入口发布 | 无（调用方拿真原函数 → 永久递归） | 先发桩再写 `B`，`callthru` 出参 |
| DEFEX | 两个执法点全局关 | 只放行加载者与 ksu 域 root |
| Kconfig 开关 | 无 | 无（一致） |

### 构建与补丁

- 三个 KMI（DDK android13-5.15 / android14-6.1 / android15-6.6，`CONFIG_KSU=m`
  `CONFIG_KSU_SAMSUNG_{KDP,DEFEX}=y`，`KSU_VERSION=32680`）：0 warning / 0 error
- 未定义符号 237 个，S23U chc 5.15 的 kallsyms 全部可解析
- 正向打到干净 v3.3.1、反向 `git apply -R` 都能过；打完的树与工作树逐文件一致
- `git diff <tag> -- kernel userspace` 与这两份补丁文件逐字节相同

## 只能上机确认的

1. `prepare_ro_creds()` 返回的 `ro_cred` 直接挂到 `real_cred`/`cred` 两个指针上，靠它自带"双引用"；
   手里没有三星 `kdp_creds.c` 源码，静态证明不了（同源代码在 YukiSU 线跑过没崩）。
2. 一条分支没能还原时，卸载后仍会跳进已释放的模块内存：`module_exit` 不能失败，只能记 `pr_err`。
3. 上游代码不动：`escape_to_root_for_adb_root()`（`selinux/selinux.c`）用裸 `commit_creds()`；
   `runtime/ksud_integration.c` 的 `stop_input_hook_work` 在 late-load 下会对零初始化的 work
   调 `schedule_work()`。

## 补丁原则

反向可回退、不用 `goto`、最小 diff、命名 `<项目>-<基线>-<用途>.patch`。
