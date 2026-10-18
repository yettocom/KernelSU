/*
 * Samsung RKP-aware function replacement.
 *
 * See include/ksu_samsung_rkp.h for why this exists.  The short version: on a target whose
 * text and rodata are taken away at EL2, ksu_patch_text()'s fixmap write is refused, so a
 * caller that wants a table entry to point somewhere else cannot write the entry.  This file
 * writes the *function* instead - one B instruction at its entry - which the kernel's own
 * text-patching path (aarch64_insn_patch_text(), the one kprobes use) does allow.
 *
 * A stub next to it keeps the original callable.  The stub has to be reached with a plain
 * call: it lives in a page no module owns, so a CFI-checked indirect call to it would fail
 * the check, and callers that reach it are marked __nocfi for that reason.
 */

#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/mutex.h>
#include <linux/moduleloader.h>
#include <linux/set_memory.h>
#include <linux/types.h>
#include <linux/version.h>
#include <linux/vmalloc.h>

#include "infra/symbol_resolver.h"
#include "ksu_samsung_rkp.h"
#include "klog.h"

#ifdef __aarch64__

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 10, 0)
#include <linux/execmem.h>
#define ksu_rkp_alloc(sz) execmem_alloc(EXECMEM_MODULE_TEXT, (sz))
#define ksu_rkp_free(p) execmem_free(p)
#else
#define ksu_rkp_alloc(sz) module_alloc(sz)
#define ksu_rkp_free(p) vfree(p)
#endif

#define KSU_RKP_MAX_SITES 16

/*
 * One stub is 16 bytes, entry at +4:
 *   [0] the replaced function's kCFI type hash (it sits at fn-4).  Every caller here is
 *       __nocfi so it is never consulted on this kernel; it is what would make the stub a
 *       valid indirect target on a kernel that does check
 *   [4] a landing pad: the PAC instruction that was at fn+0 when it was replaced, or BTI C
 *   [8] the instruction the branch displaced, re-executed before returning to the function
 *  [12] B back to the first instruction after the displaced one
 */
#define KSU_RKP_STUB_SIZE 16
#define KSU_RKP_STUBS_PER_PAGE (PAGE_SIZE / KSU_RKP_STUB_SIZE)

#define KSU_RKP_INSN_BTI_C 0xD503245Fu
#define KSU_RKP_INSN_PACIASP 0xD503233Fu
#define KSU_RKP_INSN_PACIBSP 0xD503237Fu
#define KSU_RKP_INSN_B_OPC 0x14000000u

typedef int (*ksu_rkp_insn_patch_t)(void *addrs[], u32 insns[], int cnt);

struct ksu_rkp_site {
    void *fn;
    u32 insn_at_patch; /* what the branch displaced, put back on restore */
    u32 insn_at_fn0;   /* the PAC instruction at fn+0 that was replaced, or 0 */
    unsigned int stub_idx;
    unsigned int preamble_skip;
};

static ksu_rkp_insn_patch_t ksu_rkp_insn_patch;
static struct ksu_rkp_site ksu_rkp_sites[KSU_RKP_MAX_SITES];
static unsigned int ksu_rkp_site_count;
static DEFINE_MUTEX(ksu_rkp_site_lock);
static void *ksu_rkp_stub_page;
static unsigned int ksu_rkp_stub_next;
/* Slots a restore handed back, reused only after a drain (see ksu_rkp_alloc_stub()). */
static unsigned int ksu_rkp_stub_free[KSU_RKP_MAX_SITES];
static unsigned int ksu_rkp_stub_free_count;
/* synchronize_rcu_tasks() is not exported to modules, so it is resolved by name - the same
 * reason susfs resolves it.  Without it no stub is recycled and the page is never freed.
 */
static void (*ksu_rkp_drain_callers)(void);
static bool ksu_rkp_active;

bool ksu_samsung_rkp_active(void)
{
    return READ_ONCE(ksu_rkp_active);
}

/* A B instruction, or 0 when @to is out of the +/-128 MB a B can reach. */
static u32 ksu_rkp_encode_b(void *from, void *to)
{
    s64 delta = (s64)(unsigned long)to - (s64)(unsigned long)from;
    s64 imm26 = delta >> 2;

    if (delta & 0x3)
        return 0;
    if (imm26 < -(1LL << 25) || imm26 > (1LL << 25) - 1)
        return 0;

    return KSU_RKP_INSN_B_OPC | ((u32)imm26 & 0x3FFFFFFu);
}

static bool ksu_rkp_is_pac_signing(u32 insn)
{
    return insn == KSU_RKP_INSN_PACIASP || insn == KSU_RKP_INSN_PACIBSP;
}

/* BTI variants are bits[31:8] = 0xD50324 with an even op2 and bits[4:0] = 0x1F; the PAC
 * instructions are landing pads too, which is why they are accepted here. */
static bool ksu_rkp_is_landing_pad(u32 insn)
{
    return (insn & 0xFFFFFF3Fu) == 0xD503241Fu || ksu_rkp_is_pac_signing(insn);
}

/* An instruction whose meaning depends on the address it sits at cannot be moved into a
 * stub: it would compute against the stub instead of the function. */
static bool ksu_rkp_is_pc_relative(u32 insn)
{
    return ((insn & 0x1F000000u) == 0x10000000u) || /* ADR / ADRP        */
           ((insn & 0x3B000000u) == 0x18000000u) || /* LDR literal       */
           ((insn & 0x7C000000u) == 0x14000000u) || /* B / BL            */
           ((insn & 0xFE000000u) == 0x54000000u) || /* B.cond / BC.cond  */
           ((insn & 0x7E000000u) == 0x34000000u) || /* CBZ / CBNZ        */
           ((insn & 0x7E000000u) == 0x36000000u);   /* TBZ / TBNZ        */
}

static struct ksu_rkp_site *ksu_rkp_find_site(void *fn)
{
    unsigned int i;

    for (i = 0; i < ksu_rkp_site_count; i++) {
        if (ksu_rkp_sites[i].fn == fn)
            return &ksu_rkp_sites[i];
    }

    return NULL;
}

/*
 * Put a site back exactly as it was found.  When fn+0 was replaced, both words go back in one
 * patch - the same way they went in.  Restoring them separately would leave a window in which
 * fn+0 is the original again while the branch is still there, and a caller that arrives then
 * runs the function without its PAC sign and dies at the epilogue.
 */
static int ksu_rkp_restore_site(struct ksu_rkp_site *site)
{
    void *patch_addr = (u8 *)site->fn + site->preamble_skip * 4;
    int ret;

    if (site->insn_at_fn0) {
        void *addrs[2] = { patch_addr, site->fn };
        u32 insns[2] = { site->insn_at_patch, site->insn_at_fn0 };

        ret = ksu_rkp_insn_patch(addrs, insns, 2);
    } else {
        u32 insn = site->insn_at_patch;

        ret = ksu_rkp_insn_patch(&patch_addr, &insn, 1);
    }

    if (ret)
        pr_err("ksu_samsung_rkp: restore at %pS failed: %d\n", site->fn, ret);

    return ret;
}

/*
 * Fill a stub.  The page is read-only and executable, so the words go in through the same
 * path kprobes use for their instruction slots - which also does the cache maintenance a
 * bare store into a fresh executable page would need.  All four words go in as one patch,
 * so a stub is never reachable half-written.
 */
static int ksu_rkp_fill_stub(void *stub, void *fn, void *resume, u32 displaced, u32 landing)
{
    void *addrs[4];
    u32 insns[4];
    u32 b_resume = ksu_rkp_encode_b((u8 *)stub + 12, resume);
    unsigned int i;

    if (!b_resume) {
        pr_err("ksu_samsung_rkp: stub for %pS cannot reach %pS\n", fn, resume);
        return -ERANGE;
    }

    memcpy(&insns[0], (u8 *)fn - sizeof(u32), sizeof(insns[0]));
    insns[1] = landing;
    insns[2] = displaced;
    insns[3] = b_resume;

    for (i = 0; i < ARRAY_SIZE(insns); i++)
        addrs[i] = (u8 *)stub + i * sizeof(u32);

    return ksu_rkp_insn_patch(addrs, insns, ARRAY_SIZE(insns));
}

static int ksu_rkp_ensure_stub_page(void)
{
    int ret;

    if (ksu_rkp_stub_page)
        return 0;

    ksu_rkp_stub_page = ksu_rkp_alloc(PAGE_SIZE);
    if (!ksu_rkp_stub_page)
        return -ENOMEM;

    /* A page that is still writable must not be handed out as code. */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 3, 0)
    ret = set_memory_rox((unsigned long)ksu_rkp_stub_page, 1);
#else
    ret = set_memory_ro((unsigned long)ksu_rkp_stub_page, 1);
    if (!ret)
        ret = set_memory_x((unsigned long)ksu_rkp_stub_page, 1);
#endif
    if (ret) {
        ksu_rkp_free(ksu_rkp_stub_page);
        ksu_rkp_stub_page = NULL;
    }

    return ret;
}

/*
 * Hand out a stub slot.
 *
 * A slot that a restore handed back may still be executing on another CPU: the callers that took
 * it through callthru_* keep the address, and one of them can be inside it right now.  Every
 * reuse therefore drains those callers first - one drain covers every slot freed so far, and the
 * extra one on a later reuse only costs time on a path that runs at feature-toggle speed.  Without
 * that primitive nothing is recycled, which brings back the old "page runs out" limit instead of
 * rewriting a stub under a live caller.
 *
 * A slot is consumed as soon as its stub is filled, not when the branch lands: a patch that then
 * fails leaves the stub describing the original, and because the slot is never handed out again a
 * caller that already took its address keeps calling the original rather than somebody else's
 * displaced instruction.
 */
static int ksu_rkp_alloc_stub(unsigned int *idx)
{
    if (ksu_rkp_stub_free_count && ksu_rkp_drain_callers) {
        ksu_rkp_drain_callers();
        *idx = ksu_rkp_stub_free[--ksu_rkp_stub_free_count];
        return 0;
    }

    if (ksu_rkp_stub_next >= KSU_RKP_STUBS_PER_PAGE)
        return -ENOMEM;

    *idx = ksu_rkp_stub_next++;
    return 0;
}

/* Give a restored site's slot back.  At most KSU_RKP_MAX_SITES sites exist at once, so this list
 * cannot overflow.
 */
static void ksu_rkp_free_stub(unsigned int idx)
{
    if (ksu_rkp_stub_free_count < ARRAY_SIZE(ksu_rkp_stub_free))
        ksu_rkp_stub_free[ksu_rkp_stub_free_count++] = idx;
}

/*
 * Replace the function @dst points at with the function @src points at.
 *
 * Both are pointers to function pointers, which is ksu_patch_text()'s shape; reading through
 * them here is what lets patch_memory.c forward an 8-byte slot write without knowing anything
 * else about the caller.
 */
int ksu_samsung_rkp_branch_patch_callthru(void *dst, void *src, void **callthru)
{
    void *fn;
    void *new_fn;
    struct ksu_rkp_site *site;
    void *patch_addr;
    void *resume;
    void *stub;
    u32 fn0_insn;
    u32 displaced;
    u32 landing;
    u32 branch;
    unsigned int preamble_skip;
    unsigned int stub_idx;
    int ret;

    if (!ksu_rkp_active || !dst || !src)
        return -EOPNOTSUPP;

    fn = *(void **)dst;
    new_fn = *(void **)src;
    if (!fn || !new_fn)
        return -EINVAL;

    mutex_lock(&ksu_rkp_site_lock);

    site = ksu_rkp_find_site(fn);
    if (site) {
        unsigned int old_idx = site->stub_idx;

        /* A second request for the same function replaces the first: that is the
         * re-enable-after-a-failed-disable path.  The caller that already took the old stub keeps
         * working - that stub still runs the displaced instruction and resumes in the original -
         * and its slot goes back to the pool for a drained reuse. */
        ret = ksu_rkp_restore_site(site);
        if (ret) {
            mutex_unlock(&ksu_rkp_site_lock);
            return ret;
        }

        *site = ksu_rkp_sites[--ksu_rkp_site_count];
        ksu_rkp_free_stub(old_idx);
        pr_info("ksu_samsung_rkp: branch site at %pS removed\n", fn);
        if (new_fn == fn) {
            mutex_unlock(&ksu_rkp_site_lock);
            return 0;
        }
    }

    if (fn == new_fn) {
        mutex_unlock(&ksu_rkp_site_lock);
        return 0;
    }

    if (ksu_rkp_site_count >= KSU_RKP_MAX_SITES) {
        mutex_unlock(&ksu_rkp_site_lock);
        pr_err("ksu_samsung_rkp: branch site table is full\n");
        return -ENOMEM;
    }

    ret = ksu_rkp_ensure_stub_page();
    if (ret) {
        mutex_unlock(&ksu_rkp_site_lock);
        pr_err("ksu_samsung_rkp: no executable page for stubs: %d\n", ret);
        return ret;
    }

    ret = ksu_rkp_alloc_stub(&stub_idx);
    if (ret) {
        mutex_unlock(&ksu_rkp_site_lock);
        pr_err("ksu_samsung_rkp: no stub slot available: %d\n", ret);
        return ret;
    }

    fn0_insn = *(u32 *)fn;
    displaced = fn0_insn;

    /* Step over the BTI/PAC preamble (at most two instructions) so the branch does not
     * clobber the landing pad: an indirect call arrives with BLR, and a function whose
     * fn+0 no longer takes that call raises a BTI violation instead of running.  A PAC is
     * skippable only at fn+0: the stub replays the displaced instruction, so a PAC reached
     * later in the preamble (the "bti c; paciasp" layout) has to become that displaced one
     * - step over it and the stub jumps to fn+8 with LR never signed. */
    preamble_skip = 0;
    while (preamble_skip < 2 && ksu_rkp_is_landing_pad(displaced)) {
        if (preamble_skip > 0 && ksu_rkp_is_pac_signing(displaced))
            break;
        preamble_skip++;
        displaced = *(u32 *)((u8 *)fn + preamble_skip * 4);
    }

    patch_addr = (u8 *)fn + preamble_skip * 4;
    resume = (u8 *)fn + (preamble_skip + 1) * 4;

    if (ksu_rkp_is_pc_relative(displaced)) {
        mutex_unlock(&ksu_rkp_site_lock);
        pr_err("ksu_samsung_rkp: %pS: instruction at +%u is PC-relative, not installing\n", fn,
               preamble_skip * 4);
        return -EINVAL;
    }

    /* PACIASP/PACIBSP at fn+0 signs LR in place.  Left there, it would sign before the
     * branch, and a replacement that signs again in its own entry makes AUTXSP fail at its
     * epilogue - a fatal FPAC exception.  BTI C keeps the landing pad without signing. */
    landing = KSU_RKP_INSN_BTI_C;
    if (preamble_skip > 0 && ksu_rkp_is_pac_signing(fn0_insn))
        landing = fn0_insn;

    branch = ksu_rkp_encode_b(patch_addr, new_fn);
    if (!branch) {
        mutex_unlock(&ksu_rkp_site_lock);
        pr_err("ksu_samsung_rkp: %pS -> %pS is out of branch range\n", fn, new_fn);
        return -ERANGE;
    }

    stub = (u8 *)ksu_rkp_stub_page + stub_idx * KSU_RKP_STUB_SIZE;
    ret = ksu_rkp_fill_stub(stub, fn, resume, displaced, landing);
    if (ret) {
        mutex_unlock(&ksu_rkp_site_lock);
        return ret;
    }

    /*
     * Publish the callable entry for the displaced original BEFORE the branch goes live.  A
     * caller that arrives in between must reach the stub - which runs the instruction the branch
     * displaced and resumes past it, i.e. the original function - and not the function whose
     * entry is about to become the branch.  The other order is not a window but a permanent
     * infinite recursion on whatever path the caller saves its original from, and it is the one
     * failure this parameter exists to prevent.
     *
     * The stub is a valid entry in both states, so a caller that keeps it never has to be told
     * that the branch write below failed - it just runs the original either way.
     */
    if (callthru)
        WRITE_ONCE(*callthru, (void *)((u8 *)stub + 4));

    /* fn+0 and the branch go in together: a CPU that saw one without the other would either
     * lose the landing pad or branch from a signed frame. */
    if (landing == fn0_insn && fn0_insn) {
        void *addrs[2] = { fn, patch_addr };
        u32 insns[2] = { KSU_RKP_INSN_BTI_C, branch };

        ret = ksu_rkp_insn_patch(addrs, insns, 2);
    } else {
        ret = ksu_rkp_insn_patch(&patch_addr, &branch, 1);
    }

    if (ret) {
        mutex_unlock(&ksu_rkp_site_lock);
        pr_err("ksu_samsung_rkp: patching %pS failed: %d\n", fn, ret);
        return ret;
    }

    ksu_rkp_sites[ksu_rkp_site_count].fn = fn;
    ksu_rkp_sites[ksu_rkp_site_count].insn_at_patch = displaced;
    ksu_rkp_sites[ksu_rkp_site_count].insn_at_fn0 = (landing == fn0_insn && fn0_insn) ? fn0_insn : 0;
    ksu_rkp_sites[ksu_rkp_site_count].stub_idx = stub_idx;
    ksu_rkp_sites[ksu_rkp_site_count].preamble_skip = preamble_skip;
    ksu_rkp_site_count++;

    pr_info("ksu_samsung_rkp: %pS -> %pS (stub %u)\n", fn, new_fn,
            ksu_rkp_sites[ksu_rkp_site_count - 1].stub_idx);

    mutex_unlock(&ksu_rkp_site_lock);
    return 0;
}

int ksu_samsung_rkp_branch_patch(void *dst, void *src)
{
    return ksu_samsung_rkp_branch_patch_callthru(dst, src, NULL);
}

void *ksu_samsung_rkp_get_stub(void *fn)
{
    struct ksu_rkp_site *site;
    void *stub = NULL;

    if (!ksu_rkp_active || !fn)
        return NULL;

    mutex_lock(&ksu_rkp_site_lock);
    site = ksu_rkp_find_site(fn);
    if (site && ksu_rkp_stub_page)
        stub = (u8 *)ksu_rkp_stub_page + site->stub_idx * KSU_RKP_STUB_SIZE + 4;
    mutex_unlock(&ksu_rkp_site_lock);

    return stub;
}

void ksu_samsung_rkp_init(void)
{
    ksu_rkp_insn_patch = (ksu_rkp_insn_patch_t)ksu_resolve_symbol_for_functable_hook("aarch64_insn_patch_text");

    if (!ksu_rkp_insn_patch) {
        pr_info("ksu_samsung_rkp: instruction patching is not reachable, staying off\n");
        return;
    }

    /* RKP is the reason the direct route is unavailable; without it the plain route is both
     * cheaper and better tested, so the branch route stays off. */
    if (!ksu_resolve_symbol_for_functable_hook("rkp_started")) {
        pr_info("ksu_samsung_rkp: no RKP on this kernel, the direct route is used\n");
        return;
    }

    ksu_rkp_drain_callers = (void *)ksu_resolve_symbol_for_functable_hook("synchronize_rcu_tasks");
    if (!ksu_rkp_drain_callers)
        pr_info("ksu_samsung_rkp: synchronize_rcu_tasks not reachable, stub slots are not recycled\n");

    ksu_rkp_active = true;
    pr_info("ksu_samsung_rkp: RKP detected, function slots are served by branch patching\n");
}

void ksu_samsung_rkp_exit(void)
{
    struct ksu_rkp_site *site;
    bool restored = true;

    mutex_lock(&ksu_rkp_site_lock);

    while (ksu_rkp_site_count > 0) {
        site = &ksu_rkp_sites[ksu_rkp_site_count - 1];
        if (ksu_rkp_restore_site(site)) {
            pr_warn("ksu_samsung_rkp: %pS keeps its branch\n", site->fn);
            restored = false;
        }
        ksu_rkp_site_count--;
    }

    if (ksu_rkp_stub_page && restored) {
        if (!ksu_rkp_drain_callers) {
            /* A caller that already read a callthru_* pointer can still be inside its stub, and
             * without the drain there is no way to know it has left.  Leaking one page beats
             * executing freed memory. */
            pr_err("ksu_samsung_rkp: no caller drain available, keeping the stub page\n");
        } else {
            ksu_rkp_drain_callers();
            (void)set_memory_nx((unsigned long)ksu_rkp_stub_page, 1);
            ksu_rkp_free(ksu_rkp_stub_page);
            ksu_rkp_stub_page = NULL;
            ksu_rkp_stub_next = 0;
            ksu_rkp_stub_free_count = 0;
        }
    } else if (ksu_rkp_stub_page) {
        /* A branch that is still installed reaches into the stub page, so the page has to
         * outlive the module in that case; the alternative is a jump into freed memory. */
        pr_err("ksu_samsung_rkp: a branch is still installed, keeping the stub page\n");
    }

    ksu_rkp_active = false;
    mutex_unlock(&ksu_rkp_site_lock);
}

#else /* !__aarch64__ */

bool ksu_samsung_rkp_active(void)
{
    return false;
}

int ksu_samsung_rkp_branch_patch(void *dst, void *src)
{
    return -EOPNOTSUPP;
}

int ksu_samsung_rkp_branch_patch_callthru(void *dst, void *src, void **callthru)
{
    return -EOPNOTSUPP;
}

void *ksu_samsung_rkp_get_stub(void *fn)
{
    return NULL;
}

void ksu_samsung_rkp_init(void)
{
}

void ksu_samsung_rkp_exit(void)
{
}

#endif /* __aarch64__ */
