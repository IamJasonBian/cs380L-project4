#include "types.h"
#include "x86.h"
#include "defs.h"
#include "date.h"
#include "param.h"
#include "memlayout.h"
#include "mmu.h"
#include "proc.h"

/*  
 *  This implementation of mmap uses a single linked list to track the 
 *  allocated regions of memeory.
 *  List nodes are allocated using kmalloc (from kmalloc.c)
 *  The process address space is increased using allocuvm() -vm.c
 */ 
#define NULL (mmapped_region*)0
//#define DEBUG

//prototypes for ll access helper functions
static void ll_delete(mmapped_region*, mmapped_region*);

#ifdef DEBUG
static void ll_print(void);
#endif

/* mmap creats a new mapping for the calling proc's address space
 * this function will round up to a page aligned address if needed
 * 
 * prot, flags, fd, and offset are not implemented in this version
 * of mmap
 * 
 * Inputs:  addr -  suggestion for starting address, mmap will
 *                  round up to page aligned addr if needed
 *                  (NULL --> place at any appropriate address)
 *          length- length of region to allocate (in bytes)
 *          prot -  protection level on mapped region
 *          flags - info flags for mapped region
 *          fd -    file descriptors
 *          offset- offset for a file-backed allocation    
 * (prot, flags, fd, offeset are not implemented in this version)
 *  
 * Returns: starting address (page aligned) of mapped region
 *          or (void*)-1 on failure
 */ 
void *mmap(void *addr, uint length, int prot, int flags, int fd, int offset)
{
  // Check argument inputs (only addr and length for now...)
  if (addr < (void*)0 || addr == (void*)KERNBASE || addr > (void*)KERNBASE || length < 1)
  {
    return (void*)-1;
  }

  // Get pointer to current process
  struct proc *p = myproc();

  // Expand the process' address sapce (w\ allocuvm)
  uint oldsz = p->sz;
  uint newsz = p->sz + length;
  p->sz = allocuvm(p->pgdir, oldsz, newsz);
  if (p->sz == 0)
  {
    return (void*)-1;
  }
  switchuvm(p);

  // Allocate a new region for our mmap (w/ kmalloc)
  mmapped_region* r = (mmapped_region*)kmalloc(sizeof(mmapped_region*));
  if (r == NULL)
  {
    deallocuvm(p->pgdir, newsz, oldsz);
    return (void*)-1;
  }

  // Assign list-data and meta-data to the new region
  r->start_addr = (addr = (void*)PGROUNDDOWN(oldsz));
  r->length = length;
  r->region_type = ANONYMOUS;
  r->offset = offset; 
  r->next = 0;

  //r->fd = -1;

  // Handle first call to mmap
  if (p->nregions == 0)
  {
    p->region_head = r;
  }
  else // Add region to an already existing mapped_regions list
  {
    // First check if our address is already allocated in the head node
    if (addr == p->region_head->start_addr) // conflict! Increment addr by a page (no allocation can be larger than a page?)
    {
      addr += PGROUNDDOWN(PGSIZE+length);
    }
    // Traverse the nodes in our dll to see if addr is already allocated
    mmapped_region* cursor = p->region_head;
    while (cursor->next != 0)
    {
      if (addr == cursor->start_addr)
      {
        addr += PGROUNDDOWN(PGSIZE+length);
        cursor = p->region_head; // start over, we may overlap past regions now...
      }
      else if (addr == (void*)KERNBASE || addr > (void*)KERNBASE) //we've run out of memory!
      {
        kmfree(r);
        deallocuvm(p->pgdir, newsz, oldsz);
        return (void*)-1;
      }
      cursor = cursor->next;
    }
    // Catch the final node that isn't checked in the loop
    if (addr == cursor->start_addr)
    {
      addr += PGROUNDDOWN(PGSIZE+length);
    }
    /*
    State after loop: 
      - addr does not overlap with any other allocated region
      - cursor is at the tail end of the list (cursor->next = 0)
    */

    // Add new region to the end of our mmapped_regions list
    cursor->next = r;
  }

  // Increment region count and retrun the new region's starting address
  p->nregions++;
  r->start_addr = addr;

  return r->start_addr;
}

/* munmap assumes that the address and length given will exactly
 * match an mmap node in our linked list (given that the node exits).
 * 
 * Inputs:  addr    - starting address of the region to unmap
 *          length  - length of the region to unmap
 * 
 * Returns: 0   - On success
 *          -1  - On failure
 */
int munmap(void *addr, uint length)
{
  // Sanity check on addr and length
  if (addr == (void*)KERNBASE || addr > (void*)KERNBASE || length < 1)
  {
    return -1;
  }

  struct proc *p = myproc();

  // If nothing has been allocated, there is nothing to munmap
  if (p->nregions == 0)
  {
    return -1;
  }

  // Travese our mmap dll to see if address and length are valid
  mmapped_region *prev = p->region_head;
  mmapped_region *next = p->region_head->next;
  int size = 0;

  // Check the head
  if (p->region_head->start_addr == addr && p->region_head->length)
  {
    /*deallocate the memory from the current process*/
    p->sz = deallocuvm(p->pgdir, p->sz, p->sz - length);
    switchuvm(p);
    p->nregions--;  

    if(p->region_head->next != 0)
    {
      
      size = p->region_head->next->length;
      
      ll_delete(p->region_head, 0);
      p->region_head->length = size;
    }
    else
    {
      ll_delete(p->region_head, 0);
    }

    /*return success*/
    return 0;
  }

  while(next != 0)
  {
    if (next->start_addr == addr && next->length == length)
    {
      /*deallocate the memory from the current process*/
      p->sz = deallocuvm(p->pgdir, p->sz, p->sz - length);
      switchuvm(p);
      p->nregions--;  
      
      /*remove the node from our ll*/
      size = next->next->length;
      ll_delete(next, prev);
      prev->next->length = size;
      
      /*return success*/
      return 0;
    }
    prev = next;
    next = prev->next;
  }

  // if there was no match, return -1
  return -1;
}

// Helper and Debugger fuctions ---------------

/* ll_delete removes and frees a mmapped_region node from our linked-list
 * 
 * Inputs:  node - node to be removed
 */ 
static void ll_delete (mmapped_region *node, mmapped_region *prev)
{
  if (node == myproc()->region_head)
  { 
    if(myproc()->region_head->next != 0)
    {
      myproc()->region_head = myproc()->region_head->next;
    }
    else
    {
      myproc()->region_head = 0;
    }
  }
  else
  {
    prev->next = node->next;
  }
  kmfree(node);
}

/* free_mmap_ll() deletes and frees all elements of proc's mmap linked list
 * this fuction is only called in freevm() in vm.c when the kernel space is
 * being freed.
 */ 
void free_mmap_ll()
{
  mmapped_region* r = myproc()->region_head;
  mmapped_region* temp;

  while (r != 0)
  {
    temp = r;
    ll_delete(r, 0);
    r = temp->next;
  }
}

#ifdef DEBUG
/* ll_print is a debug function that will print out the entire
 * contents of our mmap linked list for inspection.
 * 
 * Inputs:  head - head of the linked list
 */ 
static void ll_print()
{
  mmapped_region* head = myproc()->region_head;
  int n = myproc()->nregions;

  if (n == 0)
  {
    printf("Linked list is empty\n");
    return;
  }

  printf("Number of regions allocated: %d\n", n);
  printf("Head Region Address: %p\tHead Region Length: %d\n", head->start_addr, head->length);

  mmapped_region *cursor = head;
  for (int i = 1; i <= n; i++)
  {
    printf("Region #: %d\tRegion Address: %p\tRegion Length: %d\n", i, cursor->start_addr, cursor->length);
    cursor = cursor->next;
  }
}

#endif