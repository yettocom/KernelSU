#include <linux/completion.h>
#include <linux/capability.h>
#include <linux/cred.h>
#include <linux/mutex.h>
#include <linux/errno.h>
#include <linux/rcupdate.h>
#include <linux/sched.h>
#include <linux/sched/task.h>
#include <linux/sched/user.h>
#include <linux/user_namespace.h>
#include <linux/version.h>
#include <linux/workqueue.h>
#include <linux/pid.h>

#include "infra/symbol_resolver.h"
#include "ksu_samsung_kdp.h"
#include "klog.h"

#ifdef CONFIG_KSU_SAMSUNG_KDP
enum samsung_kdp_cred_command {
    KSU_SAMSUNG_KDP_COPY_CREDS = 0,
};

typedef struct cred *(*prepare_ro_creds_t)(struct cred *cred, int command, u64 task);
typedef void (*kdp_assign_pgd_t)(struct task_struct *task);
typedef void (*kdp_usecount_inc_t)(struct cred *cred);
typedef void (*kdp_set_cred_non_rcu_t)(struct cred *cred, int val);
typedef void (*set_dumpable_t)(struct mm_struct *mm, int value);
typedef void (*key_fsuid_changed_t)(struct cred *new_cred);
typedef void (*key_fsgid_changed_t)(struct cred *new_cred);
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 12, 0)
typedef unsigned int (*kdp_usecount_sub_and_test_t)(int nr, struct cred *cred);
#else
typedef unsigned int (*kdp_usecount_dec_and_test_t)(struct cred *cred);
#endif
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 11, 0)
typedef __typeof__(&inc_rlimit_ucounts) inc_rlimit_ucounts_t;
typedef __typeof__(&dec_rlimit_ucounts) dec_rlimit_ucounts_t;
#endif

struct samsung_kdp_commit_work {
    struct work_struct work;
    struct completion completion;
    struct task_struct *target;
    const struct cred *old_cred;
    struct cred *rw_cred;
    int result;
};

static prepare_ro_creds_t prepare_ro_creds_fn;
static kdp_assign_pgd_t kdp_assign_pgd_fn;
static kdp_usecount_inc_t kdp_usecount_inc_fn;
static kdp_set_cred_non_rcu_t kdp_set_cred_non_rcu_fn;
/* The rest of what commit_creds() does around the pointer swap.  Optional: a kernel without
 * them keeps the KDP path and skips that part (see ksu_samsung_kdp_init()).
 */
static set_dumpable_t set_dumpable_fn;
static key_fsuid_changed_t key_fsuid_changed_fn;
static key_fsgid_changed_t key_fsgid_changed_fn;
static int *suid_dumpable_ptr;
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 12, 0)
static kdp_usecount_sub_and_test_t kdp_usecount_sub_and_test_fn;
#else
static kdp_usecount_dec_and_test_t kdp_usecount_dec_and_test_fn;
#endif
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 11, 0)
static inc_rlimit_ucounts_t inc_rlimit_ucounts_fn;
static dec_rlimit_ucounts_t dec_rlimit_ucounts_fn;
#endif

/* Set once by ksu_samsung_kdp_init(). A device without the KDP helpers keeps
 * the standard credential path instead of failing the whole module load.
 */
static bool kdp_available;

/*
 * The transition has to run in a workqueue callback (Samsung's prepare_ro_creds() and
 * kdp_assign_pgd() may sleep) while the caller waits for it.  The work item therefore has to
 * outlive the callback: a stack object would end with the caller's frame, which is exactly what
 * complete() wakes up, so one static item plus this lock serialize the whole commit instead.
 */
static DEFINE_MUTEX(ksu_samsung_kdp_commit_lock);
static struct samsung_kdp_commit_work ksu_samsung_kdp_commit_work;

/*
 * Resolve one of the KDP helpers.  The exact lookup goes first: the variant scan accepts any
 * "<name>." / "<name>$" match, which can be a compiler fragment (.cold / .isra / .part) that
 * is not a function entry.  It is only a fallback, for the case where the plain name is gone -
 * an LTO-suffixed local, or a symbol that survives only as its .cfi_jt trampoline.
 */
static void *samsung_kdp_symbol(const char *name)
{
    void *addr = (void *)find_kernel_symbol_exact(name);

    if (!addr)
        addr = ksu_resolve_symbol_for_functable_hook(name);

    return addr;
}

static void samsung_kdp_commit_done(struct samsung_kdp_commit_work *commit_work, int result)
{
    commit_work->result = result;
    complete(&commit_work->completion);
}

static void __nocfi samsung_kdp_commit_worker(struct work_struct *work)
{
    struct samsung_kdp_commit_work *commit_work = container_of(work, struct samsung_kdp_commit_work, work);
    struct task_struct *target = commit_work->target;
    const struct cred *old_cred = commit_work->old_cred;
    const struct cred *target_cred;
    const struct cred *target_real_cred;
    struct cred *ro_cred;
    bool user_changed;

    /* The callers (KSU allowlist and the escape paths) decide whether a task
     * may gain root; this worker only performs the transition it was handed.
     * Checking current_uid() here would be meaningless, as workqueue callbacks
     * run in kernel threads.
     */
    target_cred = rcu_access_pointer(target->cred);
    target_real_cred = rcu_access_pointer(target->real_cred);
    if (target_cred != old_cred || target_real_cred != old_cred) {
        samsung_kdp_commit_done(commit_work, -EBUSY);
        return;
    }

    ro_cred = prepare_ro_creds_fn(commit_work->rw_cred, KSU_SAMSUNG_KDP_COPY_CREDS, (u64)target);
    if (!ro_cred) {
        samsung_kdp_commit_done(commit_work, -EIO);
        return;
    }

    user_changed = ro_cred->user != old_cred->user;
    if (user_changed) {
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 11, 0)
        inc_rlimit_ucounts_fn(ro_cred->ucounts, UCOUNT_RLIMIT_NPROC, 1);
#else
        atomic_inc(&ro_cred->user->processes);
#endif
    }

    /*
     * commit_creds() takes a second reference here - "we will require a ref for the subj creds
     * too" - because the two slots below are released separately by exit_creds(), so one
     * reference would be put twice.  On a KDP credential that reference has to come from
     * Samsung's own counter, the one ksu_samsung_kdp_put_cred() decrements: taking it with the
     * inline get_cred() would leave the two implementations disagreeing about one credential.
     *
     * get_cred() is not just the increment:
     *
     *     nonconst_cred->non_rcu = 0;
     *     return get_new_cred(nonconst_cred);
     *
     * and that clear has to come first, exactly as it does there.  On Samsung both fields live in
     * memory protected at EL2, so neither may be touched with a plain store: the clear goes
     * through kdp_set_cred_non_rcu() and is skipped entirely when that symbol is absent (writing
     * non_rcu directly is the same fault as the inline put_cred() this file exists to avoid).
     */
    if (kdp_set_cred_non_rcu_fn)
        kdp_set_cred_non_rcu_fn(ro_cred, 0);
    kdp_usecount_inc_fn(ro_cred);

    /*
     * The two blocks commit_creds() runs before it publishes the pointers, for the same reasons:
     * without the dumpability change a task that just gained root stays dumpable, so a process
     * of its old uid could ptrace or core-dump root memory; without the keyring update the
     * thread keyring keeps the old fsuid/fsgid.  The capability test is the kernel's
     * !cred_cap_issubset(old, new) reduced to cap_permitted - this path never changes user_ns.
     */
    if (old_cred->euid.val != ro_cred->euid.val || old_cred->egid.val != ro_cred->egid.val ||
        old_cred->fsuid.val != ro_cred->fsuid.val || old_cred->fsgid.val != ro_cred->fsgid.val ||
        !cap_issubset(old_cred->cap_permitted, ro_cred->cap_permitted)) {
        if (target->mm && set_dumpable_fn && suid_dumpable_ptr)
            set_dumpable_fn(target->mm, *suid_dumpable_ptr);
        target->pdeath_signal = 0;
        smp_wmb();
    }
    if (ro_cred->fsuid.val != old_cred->fsuid.val && key_fsuid_changed_fn)
        key_fsuid_changed_fn(ro_cred);
    if (ro_cred->fsgid.val != old_cred->fsgid.val && key_fsgid_changed_fn)
        key_fsgid_changed_fn(ro_cred);

    rcu_assign_pointer(target->real_cred, ro_cred);
    rcu_assign_pointer(target->cred, ro_cred);
    kdp_assign_pgd_fn(target);

    if (user_changed) {
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 11, 0)
        dec_rlimit_ucounts_fn(old_cred->ucounts, UCOUNT_RLIMIT_NPROC, 1);
#else
        atomic_dec(&old_cred->user->processes);
#endif
    }

    abort_creds(commit_work->rw_cred);
    commit_work->rw_cred = NULL;
    /* Two references to drop, exactly like the tail of commit_creds(): the task held this cred
     * in both real_cred and cred.  Dropping it once would leak it.
     */
    ksu_put_cred(old_cred);
    ksu_put_cred(old_cred);

    pr_info("Samsung KDP task-scoped credential install pid=%d uid=%u euid=%u\n", task_pid_nr(target),
            __kuid_val(ro_cred->uid), __kuid_val(ro_cred->euid));

    samsung_kdp_commit_done(commit_work, 0);
}
#endif

#ifdef CONFIG_KSU_SAMSUNG_KDP
/* The call below goes through an address resolved from kallsyms: __nocfi skips the kCFI type
 * check there, and noinline keeps the module's LTO from folding this back into an instrumented
 * caller, which would put the check back.
 */
void __nocfi noinline ksu_samsung_kdp_put_cred(const struct cred *cred)
{
    struct cred *mutable_cred = (struct cred *)cred;

    if (!READ_ONCE(kdp_available)) {
        put_cred(cred);
        return;
    }

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 12, 0)
    if (mutable_cred && kdp_usecount_sub_and_test_fn(1, mutable_cred))
#else
    if (mutable_cred && kdp_usecount_dec_and_test_fn(mutable_cred))
#endif
        __put_cred(mutable_cred);
}
#endif

int ksu_samsung_kdp_init(void)
{
#ifdef CONFIG_KSU_SAMSUNG_KDP
    prepare_ro_creds_fn = (prepare_ro_creds_t)samsung_kdp_symbol("prepare_ro_creds");
    kdp_assign_pgd_fn = (kdp_assign_pgd_t)samsung_kdp_symbol("kdp_assign_pgd");
    if (!prepare_ro_creds_fn || !kdp_assign_pgd_fn) {
        pr_info("Samsung KDP not present, using the standard credential path\n");
        return 0;
    }
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 12, 0)
    kdp_usecount_sub_and_test_fn = (kdp_usecount_sub_and_test_t)samsung_kdp_symbol("kdp_usecount_sub_and_test");
    if (!kdp_usecount_sub_and_test_fn) {
        pr_info("Samsung KDP not present, using the standard credential path\n");
        return 0;
    }
#else
    kdp_usecount_dec_and_test_fn = (kdp_usecount_dec_and_test_t)samsung_kdp_symbol("kdp_usecount_dec_and_test");
    if (!kdp_usecount_dec_and_test_fn) {
        pr_info("Samsung KDP not present, using the standard credential path\n");
        return 0;
    }
#endif
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 11, 0)
    inc_rlimit_ucounts_fn = (inc_rlimit_ucounts_t)samsung_kdp_symbol("inc_rlimit_ucounts");
    dec_rlimit_ucounts_fn = (dec_rlimit_ucounts_t)samsung_kdp_symbol("dec_rlimit_ucounts");
    if (!inc_rlimit_ucounts_fn || !dec_rlimit_ucounts_fn) {
        pr_info("Samsung KDP not present, using the standard credential path\n");
        return 0;
    }
#endif

    /* The increment is required, and grouped with the decrement above: Samsung's <linux/cred.h>
     * answers both fields together, and the worker below must be able to take the second
     * reference through the same counter ksu_samsung_kdp_put_cred() decrements.
     */
    kdp_usecount_inc_fn = (kdp_usecount_inc_t)samsung_kdp_symbol("kdp_usecount_inc");
    if (!kdp_usecount_inc_fn) {
        pr_info("Samsung KDP not present, using the standard credential path\n");
        return 0;
    }

    /* Optional, and only a lifetime hint: without it the second reference above is taken
     * without clearing non_rcu, which is the pre-5.13 behaviour of that helper.
     */
    kdp_set_cred_non_rcu_fn = (kdp_set_cred_non_rcu_t)samsung_kdp_symbol("kdp_set_cred_non_rcu");
    if (!kdp_set_cred_non_rcu_fn)
        pr_warn("Samsung KDP: kdp_set_cred_non_rcu missing, the second cred reference skips the non_rcu clear\n");

    /* Optional: the dumpability and thread-keyring fixups of commit_creds().  A kernel without
     * them keeps the KDP path and skips that part of the commit rather than losing KDP - the
     * plain credential path is the one that is not usable on new firmware.
     */
    set_dumpable_fn = (set_dumpable_t)samsung_kdp_symbol("set_dumpable");
    suid_dumpable_ptr = (int *)samsung_kdp_symbol("suid_dumpable");
    key_fsuid_changed_fn = (key_fsuid_changed_t)samsung_kdp_symbol("key_fsuid_changed");
    key_fsgid_changed_fn = (key_fsgid_changed_t)samsung_kdp_symbol("key_fsgid_changed");
    if (!set_dumpable_fn || !suid_dumpable_ptr || !key_fsuid_changed_fn || !key_fsgid_changed_fn)
        pr_warn("Samsung KDP: dumpability/keyring helpers incomplete, that part of the commit is skipped\n");

    kdp_available = true;
    pr_info("Samsung KDP task-scoped credential and native PGD path enabled\n");
#endif
    return 0;
}

int ksu_samsung_kdp_commit_creds(struct cred *cred)
{
#ifdef CONFIG_KSU_SAMSUNG_KDP
    struct samsung_kdp_commit_work *commit_work = &ksu_samsung_kdp_commit_work;
    bool queued;
    int ret;

    if (!cred)
        return -EINVAL;

    if (!READ_ONCE(kdp_available))
        return commit_creds(cred);

    mutex_lock(&ksu_samsung_kdp_commit_lock);
    INIT_WORK(&commit_work->work, samsung_kdp_commit_worker);
    init_completion(&commit_work->completion);
    commit_work->target = current;
    commit_work->old_cred = current_real_cred();
    commit_work->rw_cred = cred;
    commit_work->result = -EIO;

    get_task_struct(commit_work->target);
    queued = schedule_work(&commit_work->work);
    if (!queued) {
        put_task_struct(commit_work->target);
        mutex_unlock(&ksu_samsung_kdp_commit_lock);
        return -EBUSY;
    }

    wait_for_completion(&commit_work->completion);
    /* The pool bookkeeping in process_one_work() runs *after* the callback returns, so the
     * complete() above does not mean the work item is done with.  flush_work() waits for that
     * tail, which is what lets the next caller safely re-INIT this object. */
    flush_work(&commit_work->work);
    put_task_struct(commit_work->target);
    ret = commit_work->result;
    mutex_unlock(&ksu_samsung_kdp_commit_lock);
    return ret;
#else
    return commit_creds(cred);
#endif
}
