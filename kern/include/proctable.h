#ifndef _PROCARRAY_H_
#define _PROCARRAY_H_

#include <types.h>
#include <proc.h>
#include <synch.h>

struct proctable_node {
	struct proc *proc;
        pid_t parent;
	bool exited;
	int exitcode;
	struct cv *exitcv;
	struct lock *exitlock;
};
 
struct spinlock proctable_lock;
extern struct proctable_node *proctable [PID_MAX-PID_MIN];

int proctable_add(struct proc *p, pid_t *repid);
struct proctable_node *proctable_get(pid_t pid);
void proctable_update(pid_t pid);
void proctable_remove(pid_t pid);

#endif /* _PROCARRAY_H_ */

