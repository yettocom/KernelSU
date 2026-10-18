#include <linux/cred.h>
#include <linux/errno.h>
#include <linux/kprobes.h>
#include <linux/ptrace.h>
#include <linux/sched.h>

#include "arch.h"
#include "compat/samsung_defex.h"
#include "infra/symbol_resolver.h"
#include "klog.h"
#include "selinux/selinux.h"

#ifdef CONFIG_KSU_SAMSUNG_DEFEX
/* Id reported to DEFEX for tasks KSU has already rooted: DEFEX keeps its own
 * credential copy and must not observe uid 0 there. 1 is never a valid
 * application uid.
 */
#define KSU_SAMSUNG_DEFEX_HIDDEN_ID 1u

typedef void (*defex_get_task_creds_t)(struct task_struct *task, unsigned int *uid, unsigned int *fsuid,
                                       unsigned int *egid, unsigned short *cred_flags);
typedef int (*defex_set_task_creds_t)(struct task_struct *task, unsigned int uid, unsigned int fsuid,
                                      unsigned int egid, unsigned short cred_flags);

static defex_get_task_creds_t defex_get_task_creds;
static defex_set_task_creds_t defex_set_task_creds;
static bool defex_enforce_hooked;
static bool defex_user_exec_hooked;

/* Set once by ksu_samsung_defex_init(). A device without DEFEX keeps the
 * standard credential path instead of failing the whole module load.
 */
static bool defex_available;

/*
 * The task that loaded this module is DEFEX-visible before it ever enters the KSU domain -
 * ksud insmods in its own domain, not in the one KSU hands out - so the uid/domain test
 * below can never cover it and DEFEX could kill it mid-load.  Its identity is kept as
 * pid + start_time rather than a task_struct pointer: the pointer would dangle once that
 * task exits and could then name an unrelated task that reuses the memory.
 */
static struct {
    pid_t pid;
    u64 start_time;
} defex_trusted_task;

static bool defex_is_trusted(struct task_struct *task)
{
    return task && task_pid_nr(task) == defex_trusted_task.pid &&
           task->start_time == defex_trusted_task.start_time;
}

/* True when the task being checked is one DEFEX must not act on. */
static bool defex_must_not_enforce(struct task_struct *task)
{
    return defex_is_trusted(task) || (task == current && current_uid().val == 0 && is_ksu_domain());
}

/*
 * The function a probe redirects to: it answers "no violation" to whatever called the probed
 * hook without touching a single argument.  That is what makes it usable for
 * task_defex_user_exec, whose signature Samsung does not document - clearing an argument
 * there could hand the function a NULL it does not expect.
 */
static noinline int __nocfi samsung_defex_allow(void)
{
    return 0;
}

/*
 * True when @fn already ignores its arguments and returns 0: the stock Samsung stub
 * (paciasp; mov w0, wzr; ...) or the one DFRoot's module leaves behind (bti c; mov x0, #0;
 * ret).  Probing one of those only adds a trap to every call.
 */
static bool defex_fn_is_noop(const void *fn)
{
    const u32 *insn = fn;

    /* Decide on the first word before reading the next ones: the DFRoot stub this looks for is
     * three instructions (12 bytes), but a shorter stub would make the third read land in the
     * function after it (CERT ARR30-C: keep every access inside the object being examined). */
    if (insn[0] == 0xD503245Fu) /* bti c; mov x0, #0; ret */
        return insn[1] == 0xD2800000u && insn[2] == 0xD65F03C0u;

    if (insn[0] == 0xD503233Fu) /* paciasp; mov w0, wzr; ... */
        return insn[1] == 0x2A1F03E0u;

    return false;
}

static int ksu_samsung_defex_pre_handler(struct kprobe *probe, struct pt_regs *regs)
{
    struct task_struct *task = (struct task_struct *)PT_REGS_PARM1(regs);

    (void)probe;

    /*
     * Redirect the call instead of clearing its argument.  Clearing the task argument only
     * works while the vendor's function happens to check it for NULL - a device that does not
     * would dereference it and panic, and that is a guess about Samsung's code we cannot verify.
     * Jumping to a function that answers "no violation" skips the check entirely, which is the
     * same trick the user-exec probe below uses, and what diabl0w's DEFEX patch relies on
     * (validated on a real device, where it replaces the function for every task).
     */
    if (defex_must_not_enforce(task)) {
        instruction_pointer_set(regs, (unsigned long)samsung_defex_allow);
        return 1;
    }

    return 0;
}

static int ksu_samsung_defex_user_exec_pre(struct kprobe *probe, struct pt_regs *regs)
{
    (void)probe;

    /* An exec is always done by the current task, so there is no argument to read. */
    if (defex_must_not_enforce(current)) {
        instruction_pointer_set(regs, (unsigned long)samsung_defex_allow);
        return 1;
    }

    return 0;
}

static struct kprobe defex_enforce_kprobe = {
    .symbol_name = "task_defex_enforce",
    .pre_handler = ksu_samsung_defex_pre_handler,
};

static struct kprobe defex_user_exec_kprobe = {
    .symbol_name = "task_defex_user_exec",
    .pre_handler = ksu_samsung_defex_user_exec_pre,
};
#endif

int ksu_samsung_defex_init(void)
{
#ifdef CONFIG_KSU_SAMSUNG_DEFEX
    void *enforce_fn;
    void *user_exec_fn;
    int ret;

    /* Remember who loaded us: module init runs in that task's context. */
    defex_trusted_task.pid = task_pid_nr(current);
    defex_trusted_task.start_time = current->start_time;

    /*
     * Ask for the DEFEX-specific names first.  get_task_creds()/set_task_creds() are generic
     * enough to exist on a non-Samsung kernel with a different prototype, and calling such a
     * symbol through our typedef is a type mismatch the __nocfi wrappers cannot make right - they
     * only skip the kCFI check.
     */
    if (!find_kernel_symbol_exact("task_defex_enforce") &&
        !find_kernel_symbol_exact("task_defex_user_exec")) {
        pr_info("Samsung DEFEX not present, credential synchronization disabled\n");
        return 0;
    }

    defex_get_task_creds = (defex_get_task_creds_t)find_kernel_symbol_exact("get_task_creds");
    defex_set_task_creds = (defex_set_task_creds_t)find_kernel_symbol_exact("set_task_creds");
    if (!defex_get_task_creds || !defex_set_task_creds) {
        pr_info("Samsung DEFEX credential helpers missing, credential synchronization disabled\n");
        return 0;
    }

    /*
     * Probe a hook only while it still does something: on a target that already treats it as
     * inert (a stock stub, or the patch DFRoot's module leaves behind) a probe would only add
     * a trap to every call.
     */
    enforce_fn = (void *)find_kernel_symbol_exact("task_defex_enforce");
    if (!enforce_fn) {
        pr_info("Samsung DEFEX enforce hook not present, not probing it\n");
    } else if (defex_fn_is_noop(enforce_fn)) {
        pr_info("Samsung DEFEX enforce hook is already inert, not probing it\n");
    } else {
        ret = register_kprobe(&defex_enforce_kprobe);
        if (ret)
            pr_warn("Samsung DEFEX enforce hook unavailable: %d, continuing without the bypass\n", ret);
        else
            defex_enforce_hooked = true;
    }

    user_exec_fn = (void *)find_kernel_symbol_exact("task_defex_user_exec");
    if (!user_exec_fn) {
        pr_info("Samsung DEFEX user-exec hook not present, not probing it\n");
    } else if (defex_fn_is_noop(user_exec_fn)) {
        pr_info("Samsung DEFEX user-exec hook is already inert, not probing it\n");
    } else {
        ret = register_kprobe(&defex_user_exec_kprobe);
        if (ret)
            pr_warn("Samsung DEFEX user-exec hook unavailable: %d\n", ret);
        else
            defex_user_exec_hooked = true;
    }

    defex_available = true;

    pr_info("Samsung DEFEX credential synchronization and KSU-task bypass enabled\n");
#endif
    return 0;
}

void ksu_samsung_defex_exit(void)
{
#ifdef CONFIG_KSU_SAMSUNG_DEFEX
    if (defex_user_exec_hooked) {
        unregister_kprobe(&defex_user_exec_kprobe);
        defex_user_exec_hooked = false;
    }
    if (defex_enforce_hooked) {
        unregister_kprobe(&defex_enforce_kprobe);
        defex_enforce_hooked = false;
    }
#endif
}

/* Both calls below go through an address resolved from kallsyms, so they carry __nocfi for the
 * same reason the KDP helpers do: kCFI checks the type id at an indirect call site, and a
 * helper whose signature differs by a word turns the call into a kernel panic instead of a
 * failed call.  noinline keeps the module's LTO from folding this back into an instrumented
 * caller, which would put the check back; this runs on every root escalation.
 */
void __nocfi noinline ksu_samsung_defex_sync_current(void)
{
#ifdef CONFIG_KSU_SAMSUNG_DEFEX
    const struct cred *cred;
    unsigned int report_uid;
    unsigned int report_fsuid;
    unsigned int report_egid;
    unsigned short cred_flags;
    int ret;

    if (!READ_ONCE(defex_available))
        return;

    cred = current_cred();

    /* Only cred_flags is consumed from DEFEX: the ids are recomputed from the
     * live credential below, so the id out-parameters are scratch.
     */
    defex_get_task_creds(current, &report_uid, &report_fsuid, &report_egid, &cred_flags);

    if (__kuid_val(cred->euid) == 0 && __kuid_val(cred->fsuid) == 0 && __kgid_val(cred->egid) == 0) {
        report_uid = KSU_SAMSUNG_DEFEX_HIDDEN_ID;
        report_fsuid = KSU_SAMSUNG_DEFEX_HIDDEN_ID;
        report_egid = KSU_SAMSUNG_DEFEX_HIDDEN_ID;
    } else {
        report_uid = __kuid_val(cred->euid);
        report_fsuid = __kuid_val(cred->fsuid);
        report_egid = __kgid_val(cred->egid);
    }

    ret = defex_set_task_creds(current, report_uid, report_fsuid, report_egid, cred_flags);
    if (ret)
        pr_err("Samsung DEFEX credential synchronization failed: %d\n", ret);
#endif
}
