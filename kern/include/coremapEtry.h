#ifndef _COREMAPETRY_H_
#define _COREMAPETRY_H_

#include <types.h>

struct coremap_entry {
    bool available;
    int datasize;
};

struct pagetableEtry {
    vaddr_t paddr;
    int readable;
    int writeable;
    int executable;
};

#endif

