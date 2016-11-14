#include <types.h>
#include <kern/errno.h>
#include <kern/unistd.h>
#include <kern/fcntl.h>
#include <kern/wait.h>
#include <lib.h>
#include <syscall.h>
#include <synch.h>
#include <current.h>
#include <proc.h>
#include <proctable.h>
#include <spl.h>
#include <thread.h>
#include <addrspace.h>
#include <copyinout.h>
#include <vfs.h>
#include <mips/trapframe.h>
#include "opt-A2.h"

  /* this implementation of sys__exit does not do anything with the exit code */
  /* this needs to be fixed to get exit() and waitpid() working properly */

#if OPT_A2

/*
 * Entery point of the new process
 */
void
child_forkentry (void *data1, unsigned long data2)
{
	struct trapframe tf = *((struct trapframe*) data1);
	kfree(data1);
    
    curproc_setas((struct addrspace*)data2);
    // activate address space
	as_activate();
	// set return values
	tf.tf_v0 = 0;
	tf.tf_a3 = 0;

	// advance program counter
	tf.tf_epc += 4;

	// warp to usermode
	mips_usermode(&tf);
}

int
sys_fork(pid_t *retval, struct trapframe *tf)
{
	struct proc *child;
	struct trapframe *child_tf;
	int result;

	// Create new process
	child = proc_create_runprogram(curproc->p_name);
	if (child == NULL) {	
		return ENOMEM;
	}
    
	// Malloc address space
	child->p_addrspace = (struct addrspace *) kmalloc(sizeof(struct addrspace));
	if (child->p_addrspace == NULL) {
		proctable_remove(child->p_pid);
		proc_destroy(child);
		return ENOMEM;
	}

	// Copy address space
    struct addrspace *as;
	result = as_copy(curproc->p_addrspace, &as);
	if (result) {
		proc_destroy(child);
		return result;
	}
    
    //find the next available pid
    pid_t cpid;
    result = proctable_add(child, &cpid);
    if (result) {
		proc_destroy(child);
		return result;
	}
    proctable[cpid-PID_MIN]->parent = curproc->p_pid;
    spinlock_acquire(&child->p_lock);
    child->p_pid = cpid;
    spinlock_release(&child->p_lock);
	
	// Copy trapframe
	child_tf = (struct trapframe *) kmalloc(sizeof(struct trapframe));
	if (child_tf == NULL) {
		proctable_remove(child->p_pid);
		proc_destroy(child);
		return ENOMEM;
	}
	memcpy(child_tf, tf, sizeof(struct trapframe));

	// Fork new thread and attach to new process
	result = thread_fork("child thread", child, child_forkentry, (void*)child_tf, (long unsigned)as);
	if (result) {
		kfree(child_tf);
		proctable_remove(child->p_pid);
		proc_destroy(child);
		return result;
	}

	// Pass back new pid
	*retval = child->p_pid;

	return 0;
}

void sys__exit(int exitcode) {
    struct proc *p = curproc;
	pid_t pid = p->p_pid;
	struct proctable_node *pt = proctable_get(pid);

	// should never be null if this proc's thread still exists at this point
	KASSERT(pt != NULL);
    
    
    // Update exit code
    lock_acquire(pt->exitlock);
    pt->exited = true;
    pt->exitcode = _MKWAIT_EXIT(exitcode);
    
    //update the proctable, curproc's children do not need to keep exitcode
	proctable_update(pid);
    
	// Broadcast
	cv_broadcast(pt->exitcv, pt->exitlock);
    lock_release(pt->exitlock);
    
    //if curproc has no parent
    if (pt->parent == -1)
        proctable_remove(pid);

    as_deactivate();

    struct addrspace *as;
    as = curproc_setas(NULL);
    as_destroy(as);
	// Detach and destroy process
	proc_remthread(curthread);
	proc_destroy(p);
    
	thread_exit();
}



/* stub handler for getpid() system call                */
int
sys_getpid(pid_t *retval)
{
  *retval = curproc->p_pid;
  return(0);
}

/* stub handler for waitpid() system call                */

int
sys_waitpid(pid_t pid,
	    userptr_t status,
	    int options,
	    pid_t *retval)
{  
  //invalid input
  if (status == NULL) return EFAULT;
  if (options != 0) return EINVAL;
  
  //pid does not exist
  struct proctable_node *child = proctable_get(pid); 
  if (child == NULL) return ESRCH;
	
  if (curproc->p_pid != child->parent) return ECHILD;  //the process to wait for is not curproc's child


  int exitstatus;
  int result;
  
  lock_acquire(child->exitlock);
  while (!(child->exited)) {
      //wait if not exited
      cv_wait(child->exitcv, child->exitlock);
  }
  exitstatus = child->exitcode;
  result = copyout((const void *) &exitstatus, status,
			sizeof(int));
            
  lock_release(child->exitlock);

  
  if (result) {
		return EFAULT;
  }
  *retval = pid;
  return(0);
}

int sys_execv(userptr_t program, userptr_t args) {
    //error checking
    if (program == NULL) return ENOENT;
    if (args == NULL) return EFAULT;
    if (strlen((char *)program) > PATH_MAX) return E2BIG;
    
    int result;
    size_t count = 0;
    size_t total = 0;
    char **argsA= (char **) args;
    
    //count the number of arguments and check total size
    while (argsA[count]) {
        total += strlen(argsA[count])+1;
        count ++;
    }
    if (total > ARG_MAX) return E2BIG;
    
    //copy arguments to kernel stack
    char ** local_args = kmalloc((count+1)*sizeof(char *));
    if (local_args == NULL) return ENOMEM;
    int i = 0;
    size_t size = 0;
    size_t usedspace = 0;
    char * init = kmalloc(ARG_MAX);
    
    while (argsA[i]) {
        result = copyinstr((const_userptr_t) argsA[i], init+usedspace, ARG_MAX, &size);
        local_args[i] = init+usedspace;
        usedspace += size;
        i++;
        if (result) {
            return result;
        }
    }
    local_args[i] = NULL;
    
    //make a copy of the program name
    char *local_program;
    local_program = kstrdup((char *)program);
    if (local_program == NULL) return ENOMEM;
    
    struct addrspace *as;
	struct vnode *v;
	vaddr_t entrypoint, stackptr;

	/* Open the file. */
	result = vfs_open(local_program, O_RDONLY, 0, &v);
	if (result) {
		return result;
	}
    kfree(local_program);

	/* Create a new address space. */
	as = as_create();
	if (as == NULL) {
		vfs_close(v);
		return ENOMEM;
	}

	/* Switch to it and activate it. */
	struct addrspace *old = curproc_setas(as);
	as_activate();
    as_destroy(old);

	/* Load the executable. */
	result = load_elf(v, &entrypoint);
	if (result) {
		/* p_addrspace will go away when curproc is destroyed */
		vfs_close(v);
		return result;
	}

	/* Done with the file now. */
	vfs_close(v);
    
    /* Define the user stack in the address space */
	result = as_define_stack(as, &stackptr);
	if (result) {
		/* p_addrspace will go away when curproc is destroyed */
		return result;
	}
    
    //copy the argument strings to new user stack
    vaddr_t usermodePointer[count];
    for (int i=count-1; i >= 0 ; i--) {
        size_t args_size = strlen(local_args[i])+1;
        stackptr -= ROUNDUP(args_size,8);
        result = copyoutstr(local_args[i], (userptr_t)stackptr, ARG_MAX, &size);
        if (result) {
            return result;
        }
        //the new address of arguments
        usermodePointer[i] = stackptr;
    }
    usermodePointer[count] = 0;
    
    //copy the array to new user stack
    for (int i=count; i >= 0 ; i--) {
        size_t array_size = sizeof(vaddr_t);
        stackptr -= ROUNDUP(array_size, 4);
        result = copyout(&usermodePointer[i], (userptr_t)stackptr, array_size);
        if (result) {
            return result;
        }
    }
    
    kfree(init);
    kfree(local_args);
    
    vaddr_t argvAddr = 0;
    if (args) 
        argvAddr = stackptr;
    
    /* Warp to user mode. */
	enter_new_process(count /*argc*/, (userptr_t) argvAddr /*userspace addr of argv*/,
			  stackptr, entrypoint);
	
	/* enter_new_process does not return. */
	panic("enter_new_process returned\n");
	return -1;
}

#else
void sys__exit(int exitcode) {

  struct addrspace *as;
  struct proc *p = curproc;

  DEBUG(DB_SYSCALL,"Syscall: _exit(%d)\n",exitcode);

  KASSERT(curproc->p_addrspace != NULL);
  as_deactivate();
  /*
   * clear p_addrspace before calling as_destroy. Otherwise if
   * as_destroy sleeps (which is quite possible) when we
   * come back we'll be calling as_activate on a
   * half-destroyed address space. This tends to be
   * messily fatal.
   */
  as = curproc_setas(NULL);
  as_destroy(as);

  /* detach this thread from its process */
  /* note: curproc cannot be used after this call */
  proc_remthread(curthread);

  /* if this is the last user process in the system, proc_destroy()
     will wake up the kernel menu thread */
  proc_destroy(p);
  
  thread_exit();
  
  /* thread_exit() does not return, so we should never get here */
  panic("return from thread_exit in sys_exit\n");
}


/* stub handler for getpid() system call                */
int
sys_getpid(pid_t *retval)
{
  /* for now, this is just a stub that always returns a PID of 1 */
  /* you need to fix this to make it work properly */
  *retval = 1;
  return(0);
}

/* stub handler for waitpid() system call                */

int
sys_waitpid(pid_t pid,
	    userptr_t status,
	    int options,
	    pid_t *retval)
{
  int exitstatus;
  int result;

  /* this is just a stub implementation that always reports an
     exit status of 0, regardless of the actual exit status of
     the specified process.   
     In fact, this will return 0 even if the specified process
     is still running, and even if it never existed in the first place.

     Fix this!
  */

  if (options != 0) {
    return(EINVAL);
  }
  /* for now, just pretend the exitstatus is 0 */
  exitstatus = 0;
  result = copyout((void *)&exitstatus,status,sizeof(int));
  if (result) {
    return(result);
  }
  *retval = pid;
  return(0);
}
#endif

