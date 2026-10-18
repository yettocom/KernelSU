#ifndef __KSU_SAMSUNG_RKP_H
#define __KSU_SAMSUNG_RKP_H

/*
 * Samsung RKP-aware function replacement.
 *
 * RKP keeps kernel text and rodata away from the kernel at EL2, so ksu_patch_text()'s
 * fixmap write to a table slot is refused and every caller that replaces a *function
 * pointer in a table* loses its route.  This module keeps that route by doing the opposite
 * write: instead of rewriting the (read-only) slot, it puts a branch at the entry of the
 * function the slot points at, so every caller - the table included - reaches the
 * replacement.
 *
 * The replacement is a *wrapper*, not a replacement: ksu_samsung_rkp_get_stub() hands back
 * an address that runs the displaced instruction and continues in the original function, so
 * a caller that used to call the saved original keeps working.  Calling the original address
 * itself would re-enter the branch, which is why the stub exists.
 *
 * The stub is not a function of any module, so it must be reached with a plain call: a
 * CFI-checked indirect call to it fails the check and panics the kernel.  Callers that use
 * the stub are marked __nocfi.
 */

#include <linux/types.h>

/* Detects RKP once.  Never fails the caller: a kernel without it keeps the plain routes. */
void ksu_samsung_rkp_init(void);
void ksu_samsung_rkp_exit(void);

/* True once RKP was seen; the branch route is only usable then. */
bool ksu_samsung_rkp_active(void);

/*
 * Replace the function @dst points at with the function @src points at.
 * Both are pointers to function pointers, matching ksu_patch_text()'s shape.
 * 0 on success, a negative errno otherwise; every failure is logged here and leaves the
 * kernel exactly as it was found.
 */
int ksu_samsung_rkp_branch_patch(void *dst, void *src);

/*
 * The same, and on success it also publishes the callable entry for the displaced original into
 * *@callthru (when @callthru is not NULL) - before the branch becomes reachable.  A caller that
 * saved the original function pointer and calls it through that pointer needs this: once the
 * branch is in, the original's entry IS the branch, so calling it re-enters the replacement
 * forever.  What is written is the stub, which is a valid entry before the branch goes in too,
 * so the caller needs no second step and no window.
 */
int ksu_samsung_rkp_branch_patch_callthru(void *dst, void *src, void **callthru);

/* Callable entry for the function that was branch-patched, or NULL if it was not. */
void *ksu_samsung_rkp_get_stub(void *fn);

#endif /* __KSU_SAMSUNG_RKP_H */
