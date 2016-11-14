#include <types.h>
#include <proc.h>
#include <synch.h>
#include <limits.h>
#include <kern/errno.h>
#include <proctable.h>

// Global process table
struct proctable_node *proctable [PID_MAX-PID_MIN];

// Private function prototypes
struct proctable_node *proctable_create_node(struct proc *p);
int proctable_setsize(unsigned num);


/**
 * Adds a process to the process table. Returns its new pid through the second
 * parameter. Returns an error code.
 */
int
proctable_add(struct proc *p, pid_t *repid)
{
	unsigned i;
	struct proctable_node *pt;

	spinlock_acquire(&proctable_lock);

	// Create entry
	pt = proctable_create_node(p);
	if (pt == NULL) {
		spinlock_release(&proctable_lock);
		return ENOMEM;
	}

	// Find a spot in the table
	for (i = 0; i < PID_MAX-PID_MIN; i++) {
		if (proctable[i] == NULL) {
            proctable[i] = (struct proctable_node *)pt;
			*repid = i+PID_MIN;
			spinlock_release(&proctable_lock);
			return 0;
		}
	}
    
    spinlock_release(&proctable_lock);
	return ENPROC;

	panic("No vacant spot found in the process table");
}

/**
 * Creates and initializes a proctable entry.
 */
struct proctable_node *proctable_create_node(struct proc *p) {
	struct proctable_node *pt;

	pt = kmalloc(sizeof(*pt));
	if (pt == NULL) {
		return NULL;
	}

	// Set values
	pt->proc = p;
    pt->parent = -1;
	pt->exitcode = -1;
	pt->exited = false;
	
	// Create condition variable
	pt->exitcv = cv_create(pt->proc->p_name);
	if (pt->exitcv == NULL) {
		kfree(pt);
		return NULL;
	}

	// Create lock
	pt->exitlock = lock_create(pt->proc->p_name);
	if (pt->exitlock == NULL) {
		cv_destroy(pt->exitcv);
		kfree(pt);
		return NULL;
	}
	return pt;
}

/**
 * Gets the proctable_entry with the specified pid. Returns NULL if no such
 * entry was found.
 */
struct proctable_node * proctable_get(pid_t pid) {
	if (pid > PID_MAX || pid < PID_MIN) {
		return NULL;
	}
	return proctable[pid-PID_MIN];
}

/**
 * updates a process's children in process table after it exits so we can reuse 
 * the pid
 */
void proctable_update(pid_t pid) {
    for (int i=0; i<PID_MAX-PID_MIN; i++) {
        if (proctable[i] && proctable[i]->parent == pid) {
          if (proctable[i]->exited)
                proctable_remove(i+PID_MIN);
          else 
                proctable[i]->parent = -1;
          
          }
    }
}

/**
 * Removes a process from the process table. 
 */
void proctable_remove(pid_t pid) {	
	struct proctable_node *pt = proctable[pid-PID_MIN];
	KASSERT(pt != NULL);
	
    if (!lock_do_i_hold(pt->exitlock))
		lock_acquire(pt->exitlock);
        
    lock_release(pt->exitlock);
	lock_destroy(pt->exitlock);
	cv_destroy(pt->exitcv);
	kfree(pt);

	// Reclaim pid
	proctable[pid-PID_MIN] = NULL;
}


