# KernelSU 三星适配补丁（基线 `v3.3.1`）

基线：`v3.3.1`（tag `0c0fdb96`，2026-10-12 上游发布，APK code `32680`）
工作树：`D:\root\ksu-v331-work`（游离 HEAD = v3.3.1 + 两份补丁）
上一版：三份补丁（`samsung-kdp-rkp-defex` / `selinux_hide-kprobe` / `lateload`）归档在
`D:\root\_trash-20261016\old-ksu-patches\`；被换掉的 kprobe 路线源码在
`D:\root\_trash-20261016\ksu-samsung-kprobe-routes\`

| 补丁 | 字节 | 覆盖 |
|---|---|---|
| `KernelSU-v3.3.1-samsung-kdp-rkp-defex.patch` | 70,090 | `Kbuild`/`Kconfig`、`compat/samsung_{kdp,rkp,defex}`、`core/init.c`、`hook/arm64/patch_memory.c`、`hook/arm64/syscall_hook.c`、`hook/tp_marker.c`、`include/ksu_samsung_{kdp,rkp}.h`、`policy/app_profile.c`、`feature/selinux_hide.c`、`runtime/ksud_integration.c`（15 文件，+1246/-29） |
| `KernelSU-v3.3.1-samsung-lateload.patch` | 8,157 | `userspace/ksud/src/{late_load.rs,utils.rs}`（late-load 把 ksud 暂存到 `/data/local/tmp/.ksud-stage`、装完再收尾；**并传 `bundled=1`**，见下）（2 文件，+67/-24） |

合计 **17 文件 / +1516 · -55**（`git -c core.autocrlf=true diff HEAD --numstat`）。

> **版本号对齐已回归上游**（按你的要求）：`kernel/Kbuild` 的无 git fallback 回到 `-DKSU_VERSION=16`，
> `manager/build.gradle.kts` 与 `userspace/ksud/build.rs` 的 `KSU_VERSION_CODE` / `KSU_VERSION_NAME`
> 覆盖删掉，两个文件现在与 v3.3.1 逐字节相同（补丁里已不含它们）。影响面很小：
> **ko 一个字节没变**（sha256 `4e8b75b6…`，因为我们的构建树带 `.git`，版本本来就走 git 路径算出 32680）；
> 以后重编 APK 时 `versionName` 会是上游原样的 `v3.3.1`（ksud 侧仍 `trim_start_matches('v')` → `3.3.1`），
> `versionCode` 仍是 `30000 + git 提交数 = 32680`，所以管理器与内核的版本匹配不受影响。
> 唯一要记的：**无 `.git` 的环境**（拷出来的树、不带 .git 的挂载）编出来的 ko 会报版本 16。

## 核心：RKP 下不写只读槽位，改在函数入口下一条分支

RKP 在 EL2 保护内核 text 与 rodata，`ksu_patch_text()` 往表项里写会被拒——**syscall 表、
`write_op`、`sel_handle_status_ops.open`、LSM 槽这些一个字节都不写**。改法是反过来写函数：

1. `hook/arm64/patch_memory.c` 只在 `ksu_patch_text()` 最前面加一层转发（整个文件 10 行）：
   `len == sizeof(void *) && ksu_samsung_rkp_active()` → `ksu_samsung_rkp_branch_patch()`。
   其余情况仍走上游那套（`phys_from_virt()` + fixmap + `stop_machine`），非 RKP 设备行为不变。
2. `compat/samsung_rkp.c`（443 行）在**函数入口**写一条 `B`，把被顶掉的那条指令 + 回跳做成
   16 字节桩；`aarch64_insn_patch_text()` 只用来写**桩页和函数入口**。所以表项一直指向原函数，
   任何调用方（表、间接调用）都会走到替换函数。
3. 桩不在任何 module 的映射里，**必须用普通调用进入**：CFI 检查会打到 `__cfi_slowpath_diag`，
   而裸 `module_alloc` 页没有 CFI 影子 → panic。用桩的函数都标 `__nocfi`。
4. 调用方保存的"原函数"现在指向分支、不能再直接调，于是：
   - `feature/selinux_hide.c`（31/8 行）按 diabl0w 的做法引入 `callthru_*`：`orig_*` 保持真值
     （卸载时要写回），真正调用走 `callthru_*`；RKP 下 `callthru_*` = 桩。
   - `runtime/ksud_integration.c`（29/2 行）用 `ksu_samsung_rkp_get_stub()` 把
     `orig_sys_read`/`orig_sys_fstat` 换成桩。**这一步 diabl0w 没有**——见"和 diabl0w 的差别"。

关键细节（都是踩过的坑）：fn+0 若是 `PACIASP/PACIBSP`，直接留在原地会让 LR 被签两次，
替换函数尾声 `AUT` 失败 → FPAC 致命异常；做法是把 fn+0 换成 `BTI C`（合法 landing pad、不签名），
原 PAC 指令搬进桩替原函数签一次。真机上 `__arm64_sys_read`/`__arm64_sys_newfstat` 都是 `paciasp`
开头，走的正是这条。另外 fn+0 与被顶指令**合成一次** `aarch64_insn_patch_text(addrs, insns, 2)`，
不让任何 CPU 看到半成品。

## 为什么没有 `NO_PATCH_TEXT` / `KSU_SAMSUNG_RKP` 开关 / kprobe 兜底

- **开关**：分支路线无条件编译（`#ifdef __aarch64__`），运行时由 `rkp_started` 符号决定走不走。
  diabl0w 那棵树里一个 Kconfig 开关都没有，我们跟他一致。
- **kprobe 兜底**：原来"三星写不进 text/rodata"这个前提逼出来的三块，分支路线全部覆盖，
  而 diabl0w 那棵树里这些文件就是上游原样（`selinux_hide.c` 里 grep `kprobe` = 0，
  `syscall_hook_manager.c` 185 行 = 上游），所以整段回退：

| 回退 | 规模 |
|---|---|
| `feature/selinux_hide.c` 的 kprobe 路线 | 327 行 → 换成 31/8 行的 `callthru_*` |
| `hook/syscall_hook_manager.c` 的 sucompat kprobe 兜底 | 344 行 |
| `hook/arm64/syscall_hook.c` 的 RKP 早退 + 回读校验 | 44/9 行 |
| `hook/x86_64/syscall_hook.c` 的对称改动 | 38/9 行 |

sucompat 的 dispatcher 现在照上游装在一个 ni_slot 上；因为分支补丁写的是**共享的**
`__arm64_sys_ni_syscall`，所有未实现 syscall 也会进 dispatcher——`ksu_syscall_dispatcher()`
开头就是 `if (regs->syscallno != ksu_dispatcher_nr) return -ENOSYS;`，正好等于 `ni_syscall`
原本的返回值，不会递归、行为不变。

### late-load 为什么传 `bundled=1`

内核里 `ksu_bundled` 是个模块参数（`kernel/core/init.c`：`module_param_named(bundled, ksu_bundled, bool, 0)`，默认 false），
`do_get_info()` 只在它为真时才置 `KSU_GET_INFO_FLAG_BUNDLED`。管理器主页据此决定要不要给 KernelSU 那一栏挂
**"自定义"** 标签（`HomeUiState.showCustomLkmBadge = lkmMode && !isLkmBundled`）。

上游 `late_load.rs` 只传 `allow_shell=1`，注释说 jailbreak 模式下这个标志"没意义"（刷不了 boot 也就没有"内嵌 LKM 可更新"）。
我们的管理器是官方 APK、ko 是自己编的，所以那个标签本来**是准确的**；这里按需要把它去掉，做法是在那两个分支里各加 `bundled=1`：

```rust
let params = if allow_shell { cstr!("allow_shell=1 bundled=1") } else { cstr!("bundled=1") };
```

- 用的是内核本来就提供的参数，**没动内核默认值**——普通 `insmod` 仍如实报告"非内嵌"。
- **ko 一个字节没变**，三个 ko 的 SHA256 不受影响，只有 `ksud-selected` 需要重编。
- 副作用：`isLkmBundled` 变真后，`showLkmUpdate`（`isLkmBundled && ksuVersion != currentManagerVersionCode`）
  在管理器版本号 ≠ 内核版本号时会提示"有可用的内嵌 LKM 更新"。本包两边都是 32680，现在不会出现；
  换了更新的官方 APK 就会出现，而 jailbreak 模式没法照它更新——忽略即可，或把本包一起升到同版本号。

> ⚠️ **生成补丁要用 `git -c core.autocrlf=true diff <tag> -- <paths>`**：这棵工作树是 Windows git 检出，
> 全树是 CRLF，而 tag 里的 blob 是 LF。直接 `git diff` 会把**670 个文件**都算成改动（110k+ 行噪声）；
> 加 `core.autocrlf=true` 才会像 Windows 侧那样先归一化，得到真实的 17 个文件。
> 新增文件要先 `git add -N`，否则 `git diff` 看不见它们。

补丁外的东西就等于上游：`feature/sucompat.c`、`hook/syscall_hook.h`、
`hook/{syscall_hook_manager.c,x86_64/syscall_hook.c,riscv64/syscall_hook.c}`、
`manager/build.gradle.kts`、`userspace/ksud/build.rs` 都不在补丁里，逐字节等于 `v3.3.1`。
（`hook/arm64/syscall_hook.c` **在**补丁里：`patch_syscall_table()` 多一个 `callthru` 出参、按 RKP
分派，装完回读校验 dispatcher。仓库的 samsung 分支另给 `.gitattributes` 加了一行
`*.patch text eol=lf`，那是仓库自己的设置，不在补丁里。）

## 顺手修的遗留缺陷

**`-ENOENT` 传染**：`ksu_samsung_kdp_init()` / `ksu_samsung_defex_init()` 在缺三星符号时返回 `-ENOENT`，
`core/init.c` 直接 `return ret` → 带三星开关编出来的模块**在非三星设备上装不上**。

现在改成"缺符号就降级、不让模块 init 失败"：

- `compat/samsung_kdp.c`：新增 `static bool kdp_available`；缺符号/缺 ucounts 的路径改成
  `pr_info("Samsung KDP not present, using the standard credential path")` + `return 0`；
  成功时置位。`ksu_samsung_kdp_put_cred()` 与 `ksu_samsung_kdp_commit_creds()` 都加
  `if (!READ_ONCE(kdp_available))` 守卫（分别退回 `put_cred()` / `commit_creds()`）——
  否则空函数指针会被调用。
- `compat/samsung_defex.c`：同样加 `defex_available`；缺符号 → 记日志返回 0；
  **kprobe 注册失败也从 `return ret` 改成 `pr_warn` + `return 0`**（降级而不是让整个模块加载失败）；
  `ksu_samsung_defex_sync_current()` 加守卫。

## 对照 v3.3.1 源码 debug

| 检查点 | 结果 |
|---|---|
| 转发覆盖面 | arm64 上所有 `ksu_patch_text()` 调用点传的都是 8 字节函数指针槽（`sizeof(fn)` / `sizeof(patched)` / `sizeof(orig)`）→ 在 RKP 设备上全部走分支路线，直写路径不会碰到 rodata |
| `aarch64_insn_patch_text` / `rkp_started` | S23U chc/tgy 与 S24U 真机符号表里都有；ko 里两者都只按名解析（未定义符号表里没有） |
| 桩页/入口写入 | 未定义符号里没有 `aarch64_insn_write` / `set_fixmap` / `stop_machine` 之外的新依赖；`aarch64_insn_patch_text` 字符串 1 处 |
| `put_cred` 覆盖 | v3.3.1 全树只有 2 处（`core/init.c`、`hook/tp_marker.c`）→ 都已改成 `ksu_put_cred`（补丁后裸 `put_cred` 计数 = 0） |
| dispatcher 共享 `ni_syscall` | `ksu_syscall_dispatcher()` 的 `syscallno != ksu_dispatcher_nr → -ENOSYS` 守卫覆盖了"未实现 syscall 也进 dispatcher"的情况 |
| 桩上限 | 16 个站点 / 每页 256 个桩；当前用到 4（selinux_hide）+ 1（dispatcher）+ 2（ksud，boot 路径） |
| B 的 ±128MB | 真机 `_stext`=0xffffffc008140000、ko text=0xffffffc003e37fc4 → 67MB，够；超范围时返回 `-ERANGE` 只记日志、不装 |
| 安装/还原的原子性 | 两条 patch（fn+0 与分支）都是**一次** `aarch64_insn_patch_text(addrs, insns, 2)`；还原也对称，不留"fn+0 已还原、分支还在"的窗口 |
| 桩页权限 | `set_memory_rox()`（6.3+）或 `set_memory_ro()+set_memory_x()` 的返回值会检查，失败就释放该页返回错误，不把可写页当代码用 |
| 所有写入都是函数指针槽 | arm64 上 9 个 `ksu_patch_text()` 调用点全部传 `sizeof(函数指针)`；`lsm_hook.c` 里唯一非函数指针的调用（空 head 传 `&hook->list`）槽值为 NULL，会被 `-EINVAL` 拒绝 |

## 安全审查（2026-10-17，对照 KSU 源码 + diabl0w 的 ksu）

### 查过、结论是**不成立**：还原路径会不会被当成"再装一次分支"

diabl0w 的 `ksu_rkp_branch_patch()` 按**函数地址**认站点（`ksu_find_site(*(void **)dst)`），
而他树里这个函数只被调用一次（只有装、没有还原），所以这条假设他没验证过。我们这份补丁把上游
**所有** 8 字节槽写入都转发进来，其中一半是**还原**调用（`ksu_selinux_hide_unhook()`、
`ksu_lsm_unhook()`、`ksu_syscall_hook_exit()`、`ksu_unregister_syscall_hook()`），
"还原时 `*(void **)dst` 会不会已经变成替换函数自己"是必须查的一条。

结论：**不会，不用改**。分支路线下 `ksu_patch_text()` 在转发处就 `return` 了
（`hook/arm64/patch_memory.c`），**槽位一个字节都没写过**，所以还原调用读到的
`*(void **)dst` 仍是原函数 → `ksu_find_site(原函数)` 正好命中那条站点 → `restore_site()`
撤掉分支 → `new_fn == fn` 早退。于是"selinux_hide 关→开一次 `callthru_*` 变 NULL"、
"rmmod 后 syscall 表/分支指向已释放的模块内存"、"lsm 还原失效"三条都推不出来。

唯一会让 `fn == NULL` 的是 `lsm_hook.c` 里"空 head"那条非函数指针的调用，被
`if (!fn || !new_fn) return -EINVAL` 挡住（README 里原来就写了这条）。

（审查记录：我一度按"槽位已被写成替换函数"推了一遍，得出会 panic 的结论；回头把转发处的
`return` 重读一遍后推翻，代码保持原样。）

### 已修：`callthru_*` 的空指针窗口（中）

分支路线下桩是 `ksu_patch_text()` 那次调用里才建出来的，`callthru_*` 只能在它成功**之后**赋值，
于是"分支已生效、`callthru_*` 还没赋值"之间有个窗口；**首次** enable 时那里是 NULL，
uid<10000 的写入正好落进去就是调用地址 0。四个使用点都加了
`if (unlikely(!callthru_*)) return -EIO;`（窗口内拒绝这次调用，而不是崩）。第二次以后
`callthru_*` 还指着上一轮的桩，桩页到模块卸载才释放、内容一直有效，那个窗口本来就安全。

### 这一轮列的"未修"项，现在的状态（2026-10-18 收口）

1. **已修 —— boot 内嵌路径上 ksud 的 read/fstat 接线有 ~100ns 窗口**（未决项 1 的延续）：
   `ksud_integration.c` 原来是"先装分支、再把 `orig_sys_read` 换成桩"，中间一个并发 `read()` 会进
   `ksu_sys_read → orig(=原函数) → 分支 → ksu_sys_read` 无限递归。现在这个顺序被搬进 RKP 层：
   `ksu_samsung_rkp_branch_patch_callthru()` 先填桩、**再**把桩入口发布给调用方、最后才写 B，
   `ksu_syscall_table_hook()` 把 `old` 当 `callthru` 传下去，`ksud_integration.c` 里那段
   "装完再 `get_stub()` 补救"的 20 行删掉了 —— 见下面"分支安装顺带发布 callthru"。
2. **已修 —— 桩槽位是 bump 分配、不回收**：现在有空闲槽表，复用一个槽之前先
   `synchronize_rcu_tasks()` 排空"还拿着旧桩地址的调用"（`ksu_rkp_alloc_stub()`），页面不会再
   被开关次数耗光；`synchronize_rcu_tasks` 解析不到时**不回收**（退回旧行为，而不是在活着的
   调用底下改写桩），`ksu_samsung_rkp_exit()` 也是先排空再 `vfree`。
3. **仍未修（有理由）—— 还原失败时卸载仍会跳进模块**：`ksu_samsung_rkp_exit()` 只在**全部**还原成功时才释放桩页
   （比 diabl0w 无条件释放安全），但一条没撤掉的分支，目标仍是模块里的替换函数，卸载后照样是
   跳空洞；`module_exit` 不能失败，只能记 `pr_err`。正常路径不会发生（还原就是写回两条指令）。
4. **已修 —— late-load 暂存文件的 TOCTOU（低）**：`/data/local/tmp/.ksud-stage` 由 root helper
   放好后，ksud 原来直接 `rename` + `chown root` + `chmod 755` + `restorecon`，没有校验属主。
   `/data/local/tmp` 是 shell:shell 0771，只有 shell/root 能往里写——但**能写这里的 shell 本来就
   能直接跑同一目录里的 `cve-2026-43499-root` 拿 root**，所以这条不构成新的提权，只是"暂存文件
   可能被掉包"。现在 `stage_daemon_from()` 先 `symlink_metadata()` 判
   "普通文件 + `st_uid == 0` + 他人不可写"，不符就 `bail!`（保留旧 daemon），落地的文件还要再
   复核一次仍是普通文件。

### 对着真机内核复核过的假设

用 `s23-work/chc-unpacked/Image` + `_cmp/rev-disasm.sh`（`llvm-mc -disassemble`）逐条验：

| 假设 | 证据 |
|---|---|
| `task_defex_user_exec` 是空函数，不该给它挂 kprobe | 真机 `paciasp; mov w0, wzr; autiasp; ret`，正好命中 `defex_fn_is_noop()` 的第一个模式 |
| `task_defex_enforce` 清掉 PARM1 会提前返回 0 | 入口 `cbz x0, #84`，落点就是 `mov w0, w22`（w22 初值 0）的返回路径 |
| `aarch64_insn_patch_text(addrs, insns, cnt)` 签名 | 真机入口 `sub sp,#48 … cmp w2,#1 … b.lt`，x0/x1/w2 对得上 |
| fn+0 是 PACIASP 时把 fn+0 换 `BTI C`、分支放 fn+4 | `__arm64_sys_read` = `paciasp; stp x29,x30,[sp,#-16]!; …`；`__arm64_sys_ni_syscall` = `paciasp; mov x0,#-38; autiasp; ret`（不是 PC 相对指令，可搬进桩） |
| 替换函数调用桩是普通 `blr`，不会二次签名 | 编出来的 ko 里 `my_write_context`/`my_setprocattr`/`ksu_sys_read` 都是 `ldr x8,[…]; blr x8`，没有 `br` |
| `__nocfi` 真的去掉了 kCFI 检查 | 同一函数在 10-13 那版 ko 里有 `sub/ror/cmp/b.hs` 检查，当前 ko 里 `blr` 前只剩 `ldr`；桩在裸 `module_alloc` 页上没有 CFI 影子，留着检查就是 panic |
| `kdp_usecount_dec_and_test()` 就是 KDP 版的 `put_cred` | 真机 `struct cred` 没有 KDP 字段（176 B，标准 5.15 布局），引用计数在受保护区域里按地址算出来，所以它对**所有** cred 都适用；`ksu_put_cred()` 的用法（含 `tp_marker.c`、worker 里连调两次）是对的 |

## 验证（2026-10-17 本轮）

- 干净 `v3.3.1` + 两份补丁 → 19/19 追踪文件逐字节等于工作树；两份 `git apply -R --check` 全 0
  （反向检查要在**打了补丁的归一化检出**里跑：这棵工作树是 CRLF，`git apply -R` 比的是内容，
  在 CRLF 树上会报 `patch does not apply`。脚本 `_cmp/ksu-verify-patch.sh` 两件事都做）
- DDK android13-5.15、`CONFIG_KSU=m` + `CONFIG_KSU_SAMSUNG_{KDP,DEFEX}=y`、`KSU_VERSION=32680`：
  `LD [M] kernelsu.ko` + BTF，**0 warning / 0 error**，ko 7,421,880 B（`_cmp/ksu-compile-log.sh`）
- 未定义符号 237 个，S23U chc 5.15 全部可解析
- 补丁 LF / 无 BOM / 新增 `goto` 0 处；超 120 列 0 处、tab 0 处（`_cmp/rkp-style-check.sh`）

## 未决项（要你拍板，或只能上机确认）

1. **只有 late-load 路径不会走 ksud 的 read/fstat 钩子**：`ksu_ksud_init()` 在 `ksu_late_loaded`
   为真时被跳过，我们的包正是这样；一旦有人把它做成 boot 内嵌模块，那两个钩子会装，此时
   靠的是 `ksu_syscall_table_hook()` 把 `old` 当 `callthru` 传下去、RKP 层在写 B **之前**就把桩
   入口发布出去，所以不会自我递归、也没有窗口（见"分支安装顺带发布 callthru"）。diabl0w 那份
   没有这段，boot 路径上会递归。
2. **`escape_to_root_for_adb_root()` 是上游代码**——`kernel/selinux/selinux.c:222`，v3.3.0 就有，
   不是我们引入的。它是第三条提权路径（adb root → adbd 转 ksu 域），用裸 `commit_creds()`，
   没走 KDP/DEFEX。**按"源码有的就不动"原则不动。**
3. **上游 bug 不代修**：`runtime/ksud_integration.c` 的 `stop_input_hook_work` 是 BSS 里的
   `static struct work_struct`，`INIT_WORK()` 只在 boot 路径执行；late-load 下
   `ksu_stop_input_hook_runtime()` 会对零初始化的 work 调 `schedule_work()` → WARN + 丢弃排队。
   属于上游代码，本轮不修；要修就是 3 行。
4. **KDP 的 `ro_cred` 引用计数只能上机确认**：`prepare_ro_creds()` 返回的 `ro_cred` 我们直接挂到
   `real_cred`/`cred` 两个指针上，没有额外 `get_cred()`——依赖三星实现返回的 cred 自带"双引用"。
   同源代码在 YukiSU 线已上机跑过没崩，但手里没有三星 `kdp_creds.c` 源码，无法静态证明。
   （`put_cred` 那一半已经查清：真机 `struct cred` 没有 KDP 字段，计数在受保护区域里按地址算，
   所以 `ksu_put_cred()` 对普通 cred 也适用。）
5. **删掉 `-I$(KSU_KERNEL_DIR)/..` 的代价**：这条当初多半是为了兜"Windows 检出把
   `kernel/include/uapi` 软链接落成普通文本文件"的情况。如果哪天某个构建环境报
   `uapi/xxx.h: No such file or directory`，先把软链接修好（构建脚本里那句
   `rm -rf kernel/include/uapi && ln -s ../../uapi kernel/include/uapi` 就是在干这个）。

## 分支安装顺带发布 callthru（修掉我们自己引入的一个窗口）

`compat/samsung_rkp.c` 新增 `ksu_samsung_rkp_branch_patch_callthru(dst, src, callthru)`：装分支时
**先填桩 → 再把桩的可调用入口写进 `*callthru` → 最后才写 B**。顺序不能反：B 先生效而调用方还
拿着"真原函数"，不是窗口而是**永久递归**（桩在两种状态下都是合法入口，所以按这个顺序，窗口里
调用者看到的是桩，跑的就是原函数体）。

用途：`hook/arm64/syscall_hook.c` 的 `patch_syscall_table(nr, fn, old)` 在 RKP 下把 `old` 交给它，
于是 `ksu_syscall_table_hook(__NR_read, ksu_sys_read, &orig_sys_read)` 保存到的**就是桩**；
`runtime/ksud_integration.c` 里原先"装完再 `get_stub()` 补救"的 20 行（以及它带来的 ~100ns 窗口）
删掉了，那个 `#include "ksu_samsung_rkp.h"` 也一起删了（不再引用）。

**这条不是上游的 bug**：上游写表项，保存的原函数不会被分支，本来就没有这个问题；是我们换载体时
引入的，而且只在 **boot 内嵌路径**（`ksu_ksud_init`，late-load 不跑）上的 `__arm64_sys_read` /
`__arm64_sys_fstat` 两条会碰到。`ksu_syscall_table_unhook()` 传 `NULL`（还原路径靠站点表自己撤），
`hooked_entries[].orig` 仍在 patch 之前捕获，所以还原用的永远是**真原函数**。

验证：0 warning / 0 error；ko 7,425,240 B（sha256 `194c091e…`）；未定义符号仍 237；两份补丁前向
（干净 v3.3.1 打上后与工作树逐字节相同）与反向 `-R --check` 都过；新增 `goto` 0、超 120 列 0。

## 与 finasu 补丁同步（2026-10-18 晚）

finasu 那份补丁这几轮改出来的东西，按"谁的补丁就照谁改"的原则同步回来了（两边同一份代码，只是
基线不同）。同步方式：`kernel/compat/samsung_{kdp,rkp,defex}.c` **整文件对齐**（它们本来就是我们
自己的文件，diff 里只有我们的改动），其余手工适配。

| 来源 | 同步过来的改动 |
|---|---|
| 早期 | `samsung_defex.c` 的 `ksu_samsung_defex_sync_current()` 加 `__nocfi noinline`（kallsyms 间接调用的 kCFI 保护）；`samsung_kdp.c` 的 `ksu_samsung_kdp_put_cred()` 加 `noinline`（LTO 会把它折进带插桩的调用方，检查又回来）；`samsung_rkp.c` 删掉没用的 `#include <linux/sizes.h>`、`set_memory_nx()` 补 `(void)`（MISRA 17.7 / ERR33-C） |
| 第五轮 | RKP preamble：**PAC 只允许在 fn+0 被跳过**（`bti c; paciasp` 布局下 stub 会丢 PAC → 尾声 `AUTIASP` → FPAC 致命异常）；`defex_fn_is_noop()` 先判第一个字再往下读（CERT ARR30-C）；DEFEX 日志把"符号不存在"和"本来就是空桩"分开 |
| 第五轮 | `samsung_kdp.c` 的 commit work 从**调用者栈**改成文件级 `static` + `DEFINE_MUTEX`（`complete()` 一响调用方就返回，而 `process_one_work()` 之后还要写 `work->data` → 栈 UAF），并加 `flush_work()`（workqueue 的收尾在回调返回之后，下一次 `INIT_WORK` 会和它抢 `work->data`） |
| 第五轮 | `policy/app_profile.c` 的 `set_cred_ucounts()` 失败补 `ret = -ENOMEM;`（原来返回 0，sucompat/supercall 会当成功） |
| 第五轮 | `userspace/ksud/src/utils.rs` 新增 `stage_daemon_from()`：先校验（普通文件 / root 所有 / 他人不可写）→ **原子 `rename`**（失败保留旧 daemon）→ 复核落地的仍是普通文件 → 最后 chown/chmod。ksu 这边**原来没有这一步**：`late_load::run()` 走的是 `utils::install()` → `stage_daemon()` 的 `remove_file` + `copy`，非原子、不校验属主，而且是在 `load_module` **之后**（进程已经是 ksu 域）才做——所以现在把它提前到 `load_module` 之前单独 stage，后半段改用 `finish_install()` |
| 第六轮 | KDP 的"第二引用"：`kdp_set_cred_non_rcu(ro_cred, 0)` + `kdp_usecount_inc_fn(ro_cred)`（= 内核 `get_cred()` 的两步，顺序一致；`non_rcu` 在三星同样是 EL2 保护字段，只能走 KDP）；补上 dumpability/keyring 收尾（`set_dumpable` + `pdeath_signal` + `smp_wmb` + `key_fsuid_changed`/`key_fsgid_changed`） |
| 第七轮 | `samsung_rkp.c` 的 stub 槽：**填桩即消费**（失败的桩永远描述原函数、不再分发）+ 空闲槽表 + **复用前 `synchronize_rcu_tasks()` 排空** + exit 前先排空再 `vfree`（没有排空原语就留着页） |
| 第七轮 | DEFEX 的 `task_defex_enforce` 改成**重定向到 0 返回桩**（不再依赖 vendor 的 NULL 检查；思路取自 diabl0w，他那边是全局关，我们只放行 KSU 自己的任务） |

两处**故意不同**：

1. **`clear_immutable()`**：finasu 那边 `stage_daemon_from()` 失败后还会 `clear_immutable()` 重试一次
   （那是 FinaSU 上游自带的 helper）。ksu 侧没有这个 helper，**没有把 FinaSU 的特性搬进来**——所以
   ksu 的 `stage_daemon_from()` 只做"校验 → 原子 rename → 复核 → chown/chmod"。
2. **`runtime/ksud_integration.c` 的那段注释**（"Nothing to fix up afterwards: … the branch helper
   publishes the callable entry …"）在 finasu 树里被删掉了，这里**保留**：它解释的正是"为什么装完
   钩子不需要再 `get_stub()` 补救"，是准确的，删了反而少一条线索。

验证（本轮实测）：

- 三个 KMI 用 DDK 编过（`CONFIG_KSU=m CONFIG_KSU_SAMSUNG_{KDP,DEFEX}=y KSU_VERSION=32680`）：
  **0 error 0 warning**，ko 7,433,136 / 6,248,680 / 3,823,296 B
- `userspace/ksud` 的 Rust 改动**编过**：`cargo check` 与 `cargo clippy` 都 **exit=0**
  （clippy 那次重编了 ksud，不是缓存命中），crate 的 `#![deny(clippy::all, clippy::pedantic)]` 通过；
  跑法 `D:\root\_cmp\ksu-rust-check.ps1`（对着工作树副本编，不碰 `ksu-v331-work`）
- `ksu-verify-patch.sh`：新增行 `goto` = 0；两份补丁正向打到干净 v3.3.1 都 OK；打完的检出与工作树
  **17/17 逐文件一致**（忽略 CR）；两份都能 `git apply -R` 回到发行版
- 补丁本身：CR 行 0、无 BOM、新增行超 120 列 0；`Kconfig` 里 15 处 tab 是 Kconfig 语法要求的缩进
  （不是风格问题）

## 补丁原则

反向可回退、新增代码不用 `goto`、最小 diff、命名 `<项目>-<基线>-<用途>.patch`。

## 对照源码 debug（2026-10-18，ksu 侧；与 finasu 那一轮同等要求）

本轮**没有改代码**：核对的每一条都落在"已经是对的"，改的是上面几处 README 里和代码对不上的描述。
两份补丁字节未变（70,090 / 8,157 B），所以上一轮的编译与正反向验证结论继续成立。

| 核对项 | 做法 | 结果 |
|---|---|---|
| 补丁与 finasu 是否真的同步 | 抽出 ksu 两份补丁**新增的每一行**（去 CR），逐行 `grep -Fqx` 到 finasu 树 | 17 个文件里只有 3 组不在：`ksud_integration.c` 的 6 行注释、`utils.rs` 的 rename 块（`clear_immutable` 是 FinaSU 上游特性，故意没搬）、`late_load.rs`/`utils.rs` 的注释措辞。`Kconfig` 那 15 行是"整行匹配忽略不了 tab 缩进"的假象，不是差异 → 与 README 里写的"两处故意不同"一致 |
| `late_load.rs` / `utils.rs` 的改动是不是 ksud 上游本来就有 | `git show HEAD:userspace/ksud/src/late_load.rs` 对照 | 上游 `run()` 是 `daemonize(false)` + `utils::install(None,None)`；`install()` = `stage_daemon()`（`remove_file` + `copy`）+ `finish_install()`。**没有任何 `rename`**，所以 README 里"先删旧 daemon 再 rename"是错的，已改 |
| RKP 分支路线下**还原**路径会不会变成"再装一条分支" | 逐个走 `ksu_syscall_table_unhook()` / `ksu_lsm_unhook()` / `ksu_selinux_hide_unhook()` 的调用链 | 分支路线下 `ksu_patch_text()` 在转发处就 `return`，槽位一个字节都没写 → 还原时 `*(void **)dst` 仍是原函数 → `ksu_find_site()` 命中 → `restore_site()` → `new_fn == fn` 早退。**不会自环，也不会留残留分支** |
| dispatcher 安装校验的四种情况 | 直写成功 / 直写失败 / RKP 成功 / RKP 失败，逐个代进 `ksu_syscall_hook_init()` 的条件 | 四种都判对：RKP 成功时槽位没变但 `get_stub(orig)` 非空 → 不误报；RKP 失败时两者都空 → `ksu_dispatcher_nr = -1` |
| `ksu_dispatcher_nr = -1` 是否真能停掉重定向 | `grep ksu_dispatcher_nr` | `hook/syscall_hook_manager.c:106` 有 `if (ksu_dispatcher_nr < 0)` 守卫 → 停掉，不会把 `syscallno` 写成 -1 |
| KDP 失败回退时 `cred` 的归属 | 走 `samsung_kdp_commit_worker()` 的每条失败分支 + 内核 `commit_creds()` 的语义 | worker 只在**成功**路径 `abort_creds(rw_cred)` 并置 NULL；`-EBUSY`/`-EIO`/`schedule_work` 失败都把 `cred` 留给调用方 → `escape_with_root_profile()` 落到 `out_abort_creds` 的 `abort_creds(cred)` 正确，不会双释放 |
| `ksu_samsung_{kdp,defex}_init()` 的返回值 | 读两个函数的所有出口 | 现在**恒返回 0**（缺符号是降级 + `pr_info`）→ `core/init.c` 里那两个 `if (ret) return` 是防御性死代码，保留（Kconfig help 里"不因缺符号而让模块加载失败"因此成立） |
| 桩 `[0]` 那个 `fn-4` 的字是不是笔误 | 读 `KSU_RKP_STUB_SIZE` 上方的注释 | 是 kCFI 的 type hash（`__cfi_*` 就落在 `fn-4`），有注释说明；本内核所有调用点都 `__nocfi`，所以它只是"给会检查的内核留的兼容位" |
