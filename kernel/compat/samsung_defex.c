#include <linux/cred.h>
#include <linux/errno.h>
#include <linux/kprobes.h>
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
#define SAMSUNG_DEFEX_HIDDEN_ID 1u

typedef void (*defex_get_task_creds_t)(struct task_struct *task, unsigned int *uid, unsigned int *fsuid,
                                       unsigned int *egid, unsigned short *cred_flags);
typedef int (*defex_set_task_creds_t)(struct task_struct *task, unsigned int uid, unsigned int fsuid, unsigned int egid,
                                      unsigned short cred_flags);

static defex_get_task_creds_t defex_get_task_creds;
static defex_set_task_creds_t defex_set_task_creds;
static bool defex_enforce_hooked;

/* Set once by ksu_samsung_defex_init(). A device without DEFEX keeps the
 * standard credential path instead of failing the whole module load.
 */
static bool defex_available;

static int ksu_samsung_defex_pre_handler(struct kprobe *probe, struct pt_regs *regs)
{
    struct task_struct *task = (struct task_struct *)PT_REGS_PARM1(regs);

    (void)probe;
    /* Clearing the first argument (the task) makes task_defex_enforce() bail
     * out early, which is what lets our own root tasks keep running.
     */
    if (task == current && current_uid().val == 0 && is_ksu_domain())
        PT_REGS_PARM1(regs) = 0;

    return 0;
}

static struct kprobe defex_enforce_kprobe = {
    .symbol_name = "task_defex_enforce",
    .pre_handler = ksu_samsung_defex_pre_handler,
};
#endif

int ksu_samsung_defex_init(void)
{
#ifdef CONFIG_KSU_SAMSUNG_DEFEX
    int ret;

    defex_get_task_creds = (defex_get_task_creds_t)find_kernel_symbol_exact("get_task_creds");
    defex_set_task_creds = (defex_set_task_creds_t)find_kernel_symbol_exact("set_task_creds");
    if (!defex_get_task_creds || !defex_set_task_creds) {
        pr_info("Samsung DEFEX not present, credential synchronization disabled\n");
        return 0;
    }

    ret = register_kprobe(&defex_enforce_kprobe);
    if (ret) {
        pr_warn("Samsung DEFEX enforce hook unavailable: %d, continuing without the bypass\n", ret);
        return 0;
    }
    defex_enforce_hooked = true;
    defex_available = true;

    pr_info("Samsung DEFEX credential synchronization and KSU-task bypass enabled\n");
#endif
    return 0;
}

void ksu_samsung_defex_exit(void)
{
#ifdef CONFIG_KSU_SAMSUNG_DEFEX
    if (defex_enforce_hooked) {
        unregister_kprobe(&defex_enforce_kprobe);
        defex_enforce_hooked = false;
    }
#endif
}

void ksu_samsung_defex_sync_current(void)
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
        report_uid = SAMSUNG_DEFEX_HIDDEN_ID;
        report_fsuid = SAMSUNG_DEFEX_HIDDEN_ID;
        report_egid = SAMSUNG_DEFEX_HIDDEN_ID;
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
