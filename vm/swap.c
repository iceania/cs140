#include "swap.h"
#include "frame.h"
#include <bitmap.h>
#include <string.h>
#include "threads/thread.h"
#include "threads/vaddr.h"
#include "threads/malloc.h"
#include "userprog/process.h"
#include "devices/block.h"
#include "threads/pte.h"
#include "threads/interrupt.h"
#include "threads/synch.h"
#include "devices/timer.h"
#include "threads/thread.h"

/* This bit map tracks used swap slots, each swap slot is
   4096 bytes large (I.E.) one page. When the bit is set to
   one it is allocated, otherwise it is not allocated*/
static struct bitmap* used_swap_slots;

/* Our secondary device, the SWAP, kinda like a swamp but not really
   because we can traverse it without getting wet thanks to our bitmap*/
static struct block *swap_device;

/* lock for the bitmap and swap to avoid race conditions */
static struct lock swap_slots_lock;

static struct condition swap_free_condition;

#define SECTORS_PER_SLOT 8

/* Note: Swap hash tables are in the individual processes */

/* Initializes the swap, Allocating all of the user memory
   and setting up the bit map so that we can partition it
   as we so desire*/
void swap_init (void){
	swap_device = block_get_role(BLOCK_SWAP);
	if(swap_device == NULL){
		PANIC("NO SWAP!!!!");
	}

	/* Calculate the number of swap slots supported
	   by our swap device  */
	uint32_t num_sectors = block_size(swap_device);

	uint32_t num_slots = num_sectors / SECTORS_PER_SLOT;

	used_swap_slots = bitmap_create(num_slots);

	ASSERT(used_swap_slots != NULL);

	if(used_swap_slots == NULL){
		PANIC("Couldn't allocat swap bitmap");
	}

	lock_init(&swap_slots_lock);
	cond_init(&swap_free_condition);
}

/* Takes the faulting addr and then reads the data back into main memory
   setting the PTE to point to the new location, obtained using frame.h's
   frame_get_page function which might evict something else of the data that
   just got swapped back in. */
bool swap_read_in (void *faulting_addr){
	printf("srn \n");
	struct thread *cur = thread_current();
	struct process *cur_process = cur->process;
	uint32_t *pd = cur->pagedir;
	uint32_t masked_uaddr = (uint32_t)faulting_addr & PTE_ADDR;
	size_t start_sector;
	uint8_t *kaddr_ptr;
	uint32_t i, swap_slot;
	medium_t org_medium;

	ASSERT(intr_get_level() == INTR_OFF);
	lock_acquire(&cur_process->swap_table_lock);

	/* Wait while the data finishes moving to swap atomically
	   check our medium bits */
	while(pagedir_get_medium(pd, faulting_addr) != PTE_SWAP){
		/* Wait for write to disk to complete and then atomically check
		   our medium to see if our write has completed*/
		intr_enable();
		cond_wait(&swap_free_condition, &cur_process->swap_table_lock);
		intr_disable();
	}

	intr_enable();

	/* Frame get page may read something out to swap so we must
	   release this lock*/
	lock_release(&cur_process->swap_table_lock);

	ASSERT(pagedir_get_medium(pd, faulting_addr) == PTE_SWAP);

	/* May evict a page to swap, returns a kernel virtual address*/
	void* kaddr = frame_get_page(PAL_USER, (void*)faulting_addr);
	ASSERT(kaddr != NULL);

	lock_acquire(&cur_process->swap_table_lock);

	/* Lookup the corresponding swap slot that is holding this faulting
	   addresses data */
	struct swap_entry key;
	key.uaddr = masked_uaddr;
	struct hash_elem *slot_result = hash_delete(&cur_process->swap_table,
			&key.elem);
	if(slot_result == NULL){
		/* This only happens when we have inconsistency and we are trying to
		   read back into memory data that we have yet to swap out... PANIC
		   K-UNIT!!!!*/
		PANIC("Inconsistency, expected inserted hash entry absent");
	}

	struct swap_entry *entry =
			hash_entry(slot_result, struct swap_entry, elem);

	swap_slot = entry->swap_slot;
	org_medium = entry->org_medium;
	start_sector = swap_slot * SECTORS_PER_SLOT;
	kaddr_ptr = (uint8_t*)kaddr;

	/* Free the malloced swap entry */
	free(hash_entry(slot_result, struct swap_entry, elem));

	lock_release(&cur_process->swap_table_lock);

	/* Necessary, to prevent other threads which are evicting
	   to use the swap device*/
	lock_acquire(&filesys_lock);
	/* Read the contents of this swap slot into memory */
	for(i = 0; i < SECTORS_PER_SLOT;
			i++, start_sector++,kaddr_ptr += BLOCK_SECTOR_SIZE){
		block_read(swap_device, start_sector, kaddr_ptr );
	}
	lock_release(&filesys_lock);

	/* Disable interrupts while atomically setting medium
	   dirty and clear bits*/
	intr_disable();

	/* Set the page in our pagetable to point to our new frame
	   this will set the present bit back to 1*/
	ASSERT(pagedir_set_page (pd, (void*)masked_uaddr, kaddr, true));

	/* indicate that this is in memory to the owning process*/
	pagedir_set_medium(pd, (void*)masked_uaddr, org_medium);

	/* Make sure it is read back out to swap if faulted*/
	pagedir_set_dirty(pd, (void*)masked_uaddr, true);

	intr_enable();

	ASSERT(pagedir_get_medium(pd, (void*)masked_uaddr) != PTE_SWAP);

	lock_acquire(&swap_slots_lock);
	/* Set this swap slot to usable */
	bitmap_set(used_swap_slots, swap_slot, false);
	lock_release(&swap_slots_lock);

	/* allow this frame to be freed now */
	unpin_frame_entry(kaddr);
	printf("srn done\n");
	return true;
}

/* Writes the data for the kaddr to the swap device, then saves the uaddr,
   medium and swap slot for the frame entry. */
bool swap_write_out (struct thread *cur, pid_t pid, void *uaddr, void *kaddr, medium_t medium){
	printf("swo \n");
	struct process *cur_process = cur->process;
	uint32_t *pd = cur->pagedir;
	uint32_t i;
	uint32_t masked_uaddr = (((uint32_t)uaddr & PTE_ADDR));
	uint8_t *kaddr_ptr = (uint8_t*)kaddr;
	size_t swap_slot, start_sector;

	/* Acquire the swap lock */
	lock_acquire(&swap_slots_lock);

	/* Flip the first false bit to be true */
	swap_slot = bitmap_scan_and_flip(used_swap_slots, 0, 1, false);
	if(swap_slot == BITMAP_ERROR){

		PANIC("SWAP IS FULL BABY");

	}

	lock_release(&swap_slots_lock);

	if(!process_lock(pid, &cur->process->swap_table_lock)){
		/* Process has exited so we know that we can't
		   access any of the processes memory */
		lock_acquire(&swap_slots_lock);
		bitmap_set(used_swap_slots, swap_slot, false);
		lock_release(&swap_slots_lock);
		return false;
	}

	/* We set the page to not present in memory in evict so assert it*/
	ASSERT(!pagedir_is_present(pd, uaddr));
	ASSERT(pagedir_get_medium(pd, uaddr) == PTE_SWAP_WAIT);
	ASSERT(kaddr != NULL);

	ASSERT(lock_held_by_current_thread(&cur->process->swap_table_lock));

	/* make a new frame entry to store the relevant data to this
	   swap slot*/
	struct swap_entry *new_entry = calloc(1, sizeof(struct swap_entry));
	if(new_entry == NULL){
		PANIC("KERNEL OUT OF MEMORRY");
	}

	/* Set up the entry */
	new_entry->uaddr = masked_uaddr;
	new_entry->org_medium = medium;
	new_entry->swap_slot = swap_slot;

	/* Insert this into the swap map for the process. */
	struct hash_elem *returned  = hash_insert(&cur_process->swap_table,
			&new_entry->elem);
	if(returned != NULL){
		PANIC("COLLISION USING VADDR AS KEY IN HASH TABLE");
	}

	/* Write this out to disk now so that it is saved */
	start_sector = swap_slot * SECTORS_PER_SLOT;

	lock_acquire(&filesys_lock);
	for(i = 0; i < SECTORS_PER_SLOT;
			i++, start_sector++, kaddr_ptr += BLOCK_SECTOR_SIZE){
		block_write(swap_device, start_sector, kaddr_ptr);
	}
	lock_release(&filesys_lock);

	/* Tell the process who just got this page evicted that the
	   can find it on swap, pagedir_setup_demand_page does this
	   atomically*/
	if(!pagedir_setup_demand_page(pd, uaddr, PTE_SWAP,
				masked_uaddr, true)){
		PANIC("Kernel out of memory");
	}

	/* Signal that the swap is free to be used to the thread whose
	   data we wrote out, only that process can be waiting on this
	   event so we just signal.*/
	cond_signal(&swap_free_condition, &cur->process->swap_table_lock);

	lock_release(&cur->process->swap_table_lock);
	printf("swo done\n");
	return true;
}

/* Function that hashes the individual elements in the swap hash table
   this function hashes the vaddr, because all virtual
   addresses are unique in each process-- we know that this will not
   produce collisions*/
unsigned swap_slot_hash_func (const struct hash_elem *a, void *aux UNUSED){
	return hash_bytes(&hash_entry(a, struct swap_entry, elem)->uaddr,
			sizeof (int));
}

/* Atomically destroyes the hash table*/
void destroy_swap_table(struct hash *to_destroy){
	/* Free all of the swap slots that are currently occupied
	   by this process  We need both locks because it may be
	   the case that swap write out was able to acquire our
	   swap table lock and then got preempted by this thread
	   and we go ahead and destroy the table while they are
	   holding the lock and are expecting that memory to be
	   valid still. So we must acquire the lock before
	   destroying the table. Otherwise we have a race condition */
	lock_acquire(&swap_slots_lock);
	lock_acquire(&thread_current()->process->swap_table_lock);
	hash_destroy(to_destroy, &swap_slot_destroy);
	lock_release(&thread_current()->process->swap_table_lock);
	lock_release(&swap_slots_lock);
}

/* Function to compare the individual swap hash table elements
   this function compares virtual addresses, because all virtual
   addresses are unique in each process we know that this will not
   produce collisions*/
bool swap_slot_compare (const struct hash_elem *a,
		const struct hash_elem *b, void *aux UNUSED){
	ASSERT(a != NULL);
	ASSERT(b != NULL);
	return (hash_entry(a, struct swap_entry, elem)->uaddr <
			hash_entry(b, struct swap_entry, elem)->uaddr);
}

/* call all destructor for hash_destroy */
void swap_slot_destroy (struct hash_elem *e, void *aux UNUSED){
	struct swap_entry *entry = hash_entry(e, struct swap_entry, elem);
	/* Set this swap slot to usable*/
	bitmap_set(used_swap_slots, entry->swap_slot, false);
	free(entry);
}

