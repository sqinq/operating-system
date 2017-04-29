/*
 * Copyright (c) 2000, 2001, 2002, 2003, 2004, 2005, 2008, 2009
 *	The President and Fellows of Harvard College.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the University nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE UNIVERSITY AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE UNIVERSITY OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include <types.h>
#include <kern/errno.h>
#include <lib.h>
#include <spl.h>
#include <spinlock.h>
#include <proc.h>
#include <current.h>
#include <mips/tlb.h>
#include <addrspace.h>
#include <vm.h>
#include <coremapEtry.h>
#include "opt-A3.h"

/*
 * Dumb MIPS-only "VM system" that is intended to only be just barely
 * enough to struggle off the ground.
 */

/* under dumbvm, always have 48k of user stack */
#define DUMBVM_STACKPAGES    12

#if OPT_A3    
//struct coremap_entry *coremap;
struct coremap_entry *coremap;
bool coremapready = false;
int pageNum;
paddr_t mapStart;
#endif
/*
 * Wrap rma_stealmem in a spinlock.
 */
static struct spinlock stealmem_lock = SPINLOCK_INITIALIZER;

void
vm_bootstrap(void)
{
    #if OPT_A3 
    paddr_t low, high;
    ram_getsize(&low, &high);
	   
    pageNum = (high-low)/PAGE_SIZE;
    size_t mapSize = sizeof(struct coremap_entry) * pageNum;
    int map_takes_page = (mapSize+PAGE_SIZE-1)/PAGE_SIZE;
    
    
    pageNum -= map_takes_page;
    coremap = (struct coremap_entry *) PADDR_TO_KVADDR(low);

    for (int i=0; i<pageNum; i++) {
        (coremap+i)->available = true;
        (coremap+i)->datasize = -1;
    }
    
    
    mapStart = low + map_takes_page*PAGE_SIZE;
    coremapready = true;
    #endif
}

#if OPT_A3
static
int getppageIndex (unsigned long npages) {
    KASSERT(npages != 0);
    
    unsigned long emptyPagecount = 0;
    int start = 0;
    bool found = false;
    for (int i=0; i<pageNum; i++) {
        if (coremap[i].available) {
            if (emptyPagecount == 0)
                start = i;
            emptyPagecount ++;
            if (emptyPagecount == npages) {
                found = true;
                break;
            }
        } else {
            emptyPagecount = 0;
        }
    }
    
    if (found) {
        coremap[start].datasize = npages;
        for (int i=0; i<(int)npages; i++) {
            coremap[start+i].available = false;
        }
        return start;
    }
    kprintf("Physical memory full. alloc %d\n", (int)npages);

    return -1;
}
#endif

static
paddr_t
getppages(unsigned long npages)
{
	paddr_t addr;
    
    #if OPT_A3
    if (coremapready) {
        int index = getppageIndex(npages);
        if (index == -1) return 0;
        addr = index*PAGE_SIZE + mapStart;
    } else {
        spinlock_acquire(&stealmem_lock);
        addr = ram_stealmem(npages);
        spinlock_release(&stealmem_lock);
    }
    
    #else
	spinlock_acquire(&stealmem_lock);
	addr = ram_stealmem(npages);
	spinlock_release(&stealmem_lock);
    #endif
	return addr;
}

/* Allocate/free some kernel-space virtual pages */
vaddr_t 
alloc_kpages(int npages)
{
	paddr_t pa;
	pa = getppages(npages);
	if (pa==0) {
		return 0;
	}
	return PADDR_TO_KVADDR(pa);
}

void 
free_kpages(vaddr_t addr)
{
	#if OPT_A3
    paddr_t paddr = VADDR_TO_PVADDR(addr);
    KASSERT(paddr%PAGE_SIZE == 0);
    int index = (paddr-mapStart)/PAGE_SIZE;
    
   KASSERT(coremap[index].available == false && coremap[index].datasize != -1);
    int size = coremap[index].datasize;
    for (int i=0; i<size; i++) {
        coremap[index+i].available = true;
        coremap[index+i].datasize = -1;
    }
	#else
    (void)addr;
    #endif
    return;
}

void
vm_tlbshootdown_all(void)
{
	panic("dumbvm tried to do tlb shootdown?!\n");
}

void
vm_tlbshootdown(const struct tlbshootdown *ts)
{
	(void)ts;
	panic("dumbvm tried to do tlb shootdown?!\n");
}

static
paddr_t get_paddr(vaddr_t vaddr, struct pagetableEtry* ptb, vaddr_t vbase, size_t npages) {
    vaddr_t vlookfor = vaddr - vbase;
    int page = vlookfor/PAGE_SIZE;
    (void)npages;
    return VADDR_TO_PVADDR(ptb[page].paddr)+vlookfor%PAGE_SIZE;
}


int
vm_fault(int faulttype, vaddr_t faultaddress)
{
	vaddr_t vbase1, vtop1, vbase2, vtop2, stackbase, stacktop;
	paddr_t paddr;
	int i;
	uint32_t ehi, elo;
	struct addrspace *as;
	int spl;

	faultaddress &= PAGE_FRAME;

	DEBUG(DB_VM, "dumbvm: fault: 0x%x\n", faultaddress);

	switch (faulttype) {
	    case VM_FAULT_READONLY:
            #if OPT_A3
            return 1;
            #else
            /* We always create pages read-write, so we can't get this */
            panic("dumbvm: got VM_FAULT_READONLY\n");
            #endif
	    case VM_FAULT_READ:
	    case VM_FAULT_WRITE:
		break;
	    default:
		return EINVAL;
	}

	if (curproc == NULL) {
		/*
		 * No process. This is probably a kernel fault early
		 * in boot. Return EFAULT so as to panic instead of
		 * getting into an infinite faulting loop.
		 */
		return EFAULT;
	}

	as = curproc_getas();
	if (as == NULL) {
		/*
		 * No address space set up. This is probably also a
		 * kernel fault early in boot.
		 */
		return EFAULT;
	}

	/* Assert that the address space has been set up properly. 
	KASSERT(as->as_vbase1 != 0);
	KASSERT(as->as_ptable1 != 0);
	KASSERT(as->as_npages1 != 0);
	KASSERT(as->as_vbase2 != 0);
	KASSERT(as->as_ptable2 != 0);
	KASSERT(as->as_npages2 != 0);
	KASSERT(as->as_stack != 0);
	KASSERT((as->as_vbase1 & PAGE_FRAME) == as->as_vbase1);
	KASSERT((as->as_vbase2 & PAGE_FRAME) == as->as_vbase2);
*/
	vbase1 = as->as_vbase1;
	vtop1 = vbase1 + as->as_npages1 * PAGE_SIZE;
	vbase2 = as->as_vbase2;
	vtop2 = vbase2 + as->as_npages2 * PAGE_SIZE;
	stackbase = USERSTACK - DUMBVM_STACKPAGES * PAGE_SIZE;
	stacktop = USERSTACK;

    bool changingText = false;
	if (faultaddress >= vbase1 && faultaddress < vtop1) {
		paddr = get_paddr(faultaddress, as->as_ptable1, vbase1, as->as_npages1);
        changingText = true;
	}
	else if (faultaddress >= vbase2 && faultaddress < vtop2) {
		paddr = get_paddr(faultaddress, as->as_ptable2, vbase2, as->as_npages2);
	}
	else if (faultaddress >= stackbase && faultaddress < stacktop) {
		paddr = get_paddr(faultaddress, as->as_stack, stackbase, DUMBVM_STACKPAGES);
	}
	else {
		return EFAULT;
	}

	/* make sure it's page-aligned */
	KASSERT((paddr & PAGE_FRAME) == paddr);

	/* Disable interrupts on this CPU while frobbing the TLB. */
	spl = splhigh();

    bool readonly = (changingText && as->readonlyON);
	for (i=0; i<NUM_TLB; i++) {
		tlb_read(&ehi, &elo, i);
		if (elo & TLBLO_VALID) {
			continue;
		}
		ehi = faultaddress;
		elo = paddr | TLBLO_DIRTY | TLBLO_VALID;
        if (readonly)
            elo &= ~TLBLO_DIRTY;
		DEBUG(DB_VM, "dumbvm: 0x%x -> 0x%x\n", faultaddress, paddr);
		tlb_write(ehi, elo, i);
		splx(spl);
		return 0;
	}

	ehi = faultaddress;
	elo = paddr | TLBLO_DIRTY | TLBLO_VALID;
    if (readonly)
        elo &= ~TLBLO_DIRTY;
    tlb_random(ehi, elo);
	splx(spl);
	return 0;
}

struct addrspace *
as_create(void)
{
	struct addrspace *as = kmalloc(sizeof(struct addrspace));
	if (as==NULL) {
		return NULL;
	}
    
	as->as_vbase1 = 0;
	as->as_ptable1 = 0;
	as->as_npages1 = 0;
	as->as_vbase2 = 0;
	as->as_ptable2 = 0;
	as->as_npages2 = 0;
	as->as_stack = 0;
    as->readonlyON = false;

	return as;
}

void
as_destroy(struct addrspace *as)
{
    for(size_t i=0; i<as->as_npages1; i++) {
        free_kpages(as->as_ptable1[i].paddr);
    }
    
    for(size_t i=0; i<as->as_npages2; i++) {
        free_kpages(as->as_ptable2[i].paddr);
    }
    
    for(size_t i=0; i<DUMBVM_STACKPAGES; i++) {
        free_kpages(as->as_stack[i].paddr);
    }
    kfree(as->as_ptable1);
    kfree(as->as_ptable2);
    kfree(as->as_stack);
	kfree(as);
}

void
as_activate(void)
{
	int i, spl;
	struct addrspace *as;

	as = curproc_getas();
#ifdef UW
        /* Kernel threads don't have an address spaces to activate */
#endif
	if (as == NULL) {
		return;
	}

	/* Disable interrupts on this CPU while frobbing the TLB. */
	spl = splhigh();

	for (i=0; i<NUM_TLB; i++) {
		tlb_write(TLBHI_INVALID(i), TLBLO_INVALID(), i);
	}

	splx(spl);
}

void
as_deactivate(void)
{
	/* nothing */
}

int
as_define_region(struct addrspace *as, vaddr_t vaddr, size_t sz,
		 int readable, int writeable, int executable)
{
	size_t npages; 

	/* Align the region. First, the base... */
	sz += vaddr & ~(vaddr_t)PAGE_FRAME;
	vaddr &= PAGE_FRAME;

	/* ...and now the length. */
	sz = (sz + PAGE_SIZE - 1) & PAGE_FRAME;

	npages = sz / PAGE_SIZE;

	/* We don't use these - all pages are read-write */
	(void)readable;
	(void)writeable;
	(void)executable;

	if (as->as_vbase1 == 0) {
		as->as_vbase1 = vaddr;
        as->as_ptable1 = kmalloc(npages*sizeof(struct pagetableEtry));
		if (as->as_ptable1 == 0)
            return ENOMEM;
        as->as_npages1 = npages;
		return 0;
	}

	if (as->as_vbase2 == 0) {
		as->as_vbase2 = vaddr;
        as->as_ptable2 = kmalloc(npages*sizeof(struct pagetableEtry));
		if (as->as_ptable2 == 0)
            return ENOMEM;
        as->as_npages2 = npages;
		return 0;
	}

	/*
	 * Support for more than two regions is not available.
	 */
	kprintf("dumbvm: Warning: too many regions\n");
	return EUNIMP;
}

static
void
as_zero_region(paddr_t paddr, unsigned npages)
{
	bzero((void *)PADDR_TO_KVADDR(paddr), npages * PAGE_SIZE);
}

int
as_prepare_load(struct addrspace *as)
{
	KASSERT(as->as_ptable1);
	KASSERT(as->as_ptable2);

    for(size_t i=0; i<as->as_npages1; i++) {
        as->as_ptable1[i].paddr = alloc_kpages(1);
        if (as->as_ptable1[i].paddr == 0) {
            return ENOMEM;
        }
        as_zero_region(VADDR_TO_PVADDR(as->as_ptable1[i].paddr), 1);
    }

	for(size_t i=0; i<as->as_npages2; i++) {
        as->as_ptable2[i].paddr = alloc_kpages(1);
        if (as->as_ptable2[i].paddr == 0) {
            return ENOMEM;
        }
        as_zero_region(VADDR_TO_PVADDR(as->as_ptable2[i].paddr), 1);
    }
	return 0;
}

int
as_complete_load(struct addrspace *as)
{
    #if OPT_A3
	as->readonlyON = true;
    /* Disable interrupts on this CPU while frobbing the TLB. */
	int spl = splhigh();

	for (int i=0; i<NUM_TLB; i++) {
		tlb_write(TLBHI_INVALID(i), TLBLO_INVALID(), i);
	}

	splx(spl);
    #else
    (void)as;
    #endif
	return 0;
}

int
as_define_stack(struct addrspace *as, vaddr_t *stackptr)
{
	KASSERT(as->as_stack == 0);
    
    as->as_stack = kmalloc(sizeof(struct pagetableEtry)*DUMBVM_STACKPAGES);
    if (as->as_stack == 0) {
            return ENOMEM;
    }

    for(size_t i=0; i<DUMBVM_STACKPAGES; i++) {
        as->as_stack[i].paddr = alloc_kpages(1);
        if (as->as_stack[i].paddr == 0) {
            return ENOMEM;
        }
        as_zero_region(VADDR_TO_PVADDR(as->as_stack[i].paddr), 1);
    }
    
	*stackptr = USERSTACK;
	return 0;
}

int
as_copy(struct addrspace *old, struct addrspace **ret)
{
	struct addrspace *new;
    vaddr_t userstack;

	new = as_create();
	if (new==NULL) {
		return ENOMEM;
	}

	new->as_vbase1 = old->as_vbase1;
    new->as_ptable1 = kmalloc(old->as_npages1*sizeof(struct pagetableEtry));
	if (new->as_ptable1 == 0)
        return ENOMEM;
	new->as_npages1 = old->as_npages1;
	new->as_vbase2 = old->as_vbase2;
    new->as_ptable2 = kmalloc(old->as_npages2*sizeof(struct pagetableEtry));
    if (new->as_ptable2 == 0)
        return ENOMEM;
	new->as_npages2 = old->as_npages2;
    #if OPT_A3
    new->readonlyON = old->readonlyON;
    #endif

	/* (Mis)use as_prepare_load to allocate some physical memory. */
	if (as_prepare_load(new)) {
		as_destroy(new);
		return ENOMEM;
	}
    if (as_define_stack(new, &userstack)) {
        as_destroy(new);
		return ENOMEM;
    }

	KASSERT(new->as_ptable1 != 0);
	KASSERT(new->as_ptable2 != 0);
	KASSERT(new->as_stack != 0);

    for(size_t i=0; i<old->as_npages1; i++) {
        memmove((void *)new->as_ptable1[i].paddr,
            (const void *)old->as_ptable1[i].paddr,
            PAGE_SIZE);
    }

	for(size_t i=0; i<old->as_npages2; i++) {
        memmove((void *)new->as_ptable2[i].paddr,
            (const void *)old->as_ptable2[i].paddr,
            PAGE_SIZE);
    }

    for(size_t i=0; i<DUMBVM_STACKPAGES; i++) {
        memmove((void *)new->as_stack[i].paddr,
            (const void *)old->as_stack[i].paddr,
            PAGE_SIZE);
    }

	
	*ret = new;
	return 0;
}
