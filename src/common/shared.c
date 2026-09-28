/*
 * shared.c - locate the daemon's public state block.
 */
#include <exec/types.h>
#include <exec/semaphores.h>
#include <dos/dos.h>
#include <proto/exec.h>
#include "bbs.h"

struct BBSShared *shared_find(void)
{
    struct SignalSemaphore *ss;
    struct BBSShared *s = NULL;

    Forbid();
    ss = FindSemaphore((STRPTR)BBS_SEMNAME);
    /* the semaphore is the first member, so its address is the block's */
    if (ss) {
        s = (struct BBSShared *)ss;
        if (s->magic != BBS_SHARED_MAGIC || s->version != BBS_SHARED_VER) s = NULL;
    }
    Permit();
    return s;
}

void shared_lock(struct BBSShared *s)   { ObtainSemaphore(&s->sem); }
void shared_unlock(struct BBSShared *s) { ReleaseSemaphore(&s->sem); }

/* is this Task still in exec's lists? (a crashed or ended node's pointer isn't) */
BOOL task_alive(struct Task *t)
{
    struct Node *n;
    BOOL found = FALSE;
    if (!t) return FALSE;
    Disable();
    if (SysBase->ThisTask == t) found = TRUE;
    for (n = SysBase->TaskReady.lh_Head; !found && n->ln_Succ; n = n->ln_Succ)
        if ((struct Task *)n == t) found = TRUE;
    for (n = SysBase->TaskWait.lh_Head; !found && n->ln_Succ; n = n->ln_Succ)
        if ((struct Task *)n == t) found = TRUE;
    Enable();
    return found;
}

/* A sysop RESET of node n (BBSCtl, BBSControl, the sysop menu, ARexx): hang the
 * caller up and ask the node to let go of its slot at once - a node waiting on a
 * door that won't end frees the slot and stays behind on its own, out of the way.
 * A node that doesn't answer at all is freed by the daemon NODE_RESET_FORCE
 * seconds later.  Freeing the slot also ends a single = yes door's lock. */
BOOL node_reset_start(struct BBSShared *s, int n)
{
    struct NodeInfo *ni;
    struct Task *t = NULL;
    if (n < 1 || n > s->nodes) return FALSE;
    ni = &s->node[n - 1];
    shared_lock(s);
    if (ni->state == NS_FREE) { shared_unlock(s); return FALSE; }
    ni->reset = 1;
    if (task_alive(ni->task)) t = ni->task;
    if (t) Signal(t, SIGBREAKF_CTRL_C);
    shared_unlock(s);
    return TRUE;
}
