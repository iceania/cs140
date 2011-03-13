#include "userprog/syscall.h"
#include "userprog/process.h"
#include "userprog/pagedir.h"
#include "threads/vaddr.h"
#include "threads/thread.h"
#include "threads/malloc.h"
#include "threads/pte.h"
#include "threads/interrupt.h"
#include "threads/synch.h"
#include "devices/timer.h"
#include "filesys/filesys.h"
#include "filesys/file.h"
#include "vm/frame.h"
#include "vm/mmap.h"
#include <stdio.h>


/* Wait until the PTE status goes from PTE_MMAP_WAIT to
   PTE_MMAP */
static void mmap_wait_until_saved(uint32_t *pd, void *uaddr){
	ASSERT(intr_get_level() == INTR_OFF);
	while(pagedir_get_medium(pd, uaddr) != PTE_MMAP){
		/* Wait for write to disk to complete*/
		intr_enable();
		timer_msleep(8);
		intr_disable();
	}
}

/* Saves all of the pages that are dirty for the given mmap_hash_entry
   and frees their frames. */
void mmap_save_all(struct mmap_hash_entry *entry){
	uint32_t *pd =  thread_current()->pagedir;
	struct fd_hash_entry *fd_entry = fd_to_fd_hash_entry(entry->fd);

	/* The file should never be closed as long as there is a
	   mmapping to it */
	ASSERT(fd_entry != NULL);

	fd_entry->num_mmaps --;
	/* Write all of the files out to disk */
	uint8_t* pg_ptr = (uint8_t*)entry->begin_addr;
	uint32_t j;
	void *kaddr_for_pg;

	off_t offset, write_bytes, last_page_length;

	last_page_length = PGSIZE - ((entry->num_pages*PGSIZE) - entry->length_of_file);

	/* Pin all of the frames that we are going to be
	   removing so they can not be evicted*/
	intr_disable();
	for(j = 0; j < entry->num_pages; j++, pg_ptr += PGSIZE){
		if(pagedir_get_medium(pd, pg_ptr) == PTE_MMAP_WAIT){
			/* Being written out to disk now wait till it is done*/
			mmap_wait_until_saved(pd, pg_ptr);
			/* Was just saved so continue*/
			continue;
		}

		if(pagedir_is_present(pd, pg_ptr) && pagedir_is_dirty(pd, pg_ptr) &&
				pagedir_get_medium(pd, pg_ptr) == PTE_MMAP){
			kaddr_for_pg = pagedir_get_page(pd, pg_ptr);
			intr_enable();

			if(pin_frame_entry(kaddr_for_pg)){
				/* It is now pinned so it will not be evicted */
				offset = (uint32_t) pg_ptr - entry->begin_addr;

				write_bytes = (entry->num_pages -1 == j)  ? last_page_length : PGSIZE;

				file_write_at(fd_entry->open_file, pg_ptr, write_bytes, offset);

				ASSERT(pagedir_is_present(thread_current()->pagedir, pg_ptr));
				unpin_frame_entry(kaddr_for_pg);
				intr_disable();
			}else{
				/* Some other thread beat us to it and is now
				   evicting our page, we need to wait until they
				   are done before moving onto the next page in
				   our mmapped file*/
				intr_disable();
				mmap_wait_until_saved(pd, pg_ptr);
			}
		}
	}
	intr_enable();

}

/* Read in the appropriate file block from disk
   We know that the current thread is the only one that
   can call this function, when it page faulted
   trying to access memory*/
bool mmap_read_in(void *faulting_addr){
	struct process *cur_process = thread_current()->process;
	uint32_t *pd = thread_current()->pagedir;
	/* Get the key into the hash, AKA the uaddr of this page*/
	uint32_t masked_uaddr = (uint32_t)faulting_addr & PTE_ADDR;
	uint32_t offset;
	void * kaddr;

	mmap_wait_until_saved(pd, faulting_addr);

	intr_enable();

	ASSERT(pagedir_get_medium(pd, faulting_addr) == PTE_MMAP);

	lock_acquire(&cur_process->mmap_table_lock);

	/* Get hash entry if it exists */
	struct mmap_hash_entry *entry = uaddr_to_mmap_entry(cur_process, (uint32_t*)masked_uaddr);

	lock_release(&cur_process->mmap_table_lock);

	/* If this is not true we routed the wrong thing to
	   mmap read in*/
	ASSERT(entry != NULL);

	offset = masked_uaddr - entry->begin_addr;

	/* Accessed through kernel memory the user PTE will not be
	   marked as accessed or dirty !!! */
	kaddr = frame_get_page(PAL_USER, (void*)masked_uaddr);

	ASSERT(kaddr != NULL);

	struct fd_hash_entry *fd_entry = fd_to_fd_hash_entry(entry->fd);
	ASSERT(fd_entry != NULL);

	/* The actual reading in from the block always tries to read PGSIZE
	   bytes even though the last page may have many zeros that don't
	   belong to the file. This is because it leverages the fact that
	   file_read will only read up untill the end of the file and
	   never more so we know we will only read the appropriate amount
	   of data into our zero page*/

	uint32_t read_bytes = (entry->end_addr - masked_uaddr) == PGSIZE ?
			(entry->begin_addr + entry->length_of_file) - masked_uaddr : PGSIZE;

	off_t amount_read = file_read_at(fd_entry->open_file, kaddr, read_bytes, offset);

	if(amount_read < PGSIZE){
		memset((uint8_t*)kaddr + amount_read, 0, PGSIZE - amount_read);
	}

	intr_disable();

	ASSERT(pagedir_install_page((void*)masked_uaddr, kaddr, true));

	/* Make sure that we stay consistent with our naming scheme
	   of memory*/
	pagedir_set_medium(pd, (void*)masked_uaddr, PTE_MMAP);

	/* make sure we know that this page is saved*/
	pagedir_set_dirty(pd, (void*)masked_uaddr, false);

	intr_enable();
	ASSERT(pagedir_is_present(thread_current()->pagedir, (void *)masked_uaddr));
	unpin_frame_entry(kaddr);

	return true;
}

/* uaddr is expected to be page aligned, pointing to a page
   that is used for this mmapped file */
bool mmap_write_out(struct process *cur_process, uint32_t *pd,
		pid_t pid, void *uaddr, void *kaddr){
	uint32_t masked_uaddr = (((uint32_t)uaddr & PTE_ADDR));
	if(!process_lock(pid, &cur_process->mmap_table_lock)){
		/* Process has exited so we know that we can't
		   access any of the processes memory */
		return true;
	}

	ASSERT(lock_held_by_current_thread(&cur_process->mmap_table_lock));

	/* We should have set this up atomically before being
	   called */
	ASSERT(!pagedir_is_present(pd, (void*)masked_uaddr));
	ASSERT(pagedir_get_medium(pd, (void*)masked_uaddr) == PTE_MMAP_WAIT);
	ASSERT(kaddr != NULL);

	/* An arbitrary number of threads can call into this code
	   while the owning thread changes the structure of the mmap
	   table, so both adding and removing data from the mmap table
	   and reading it from this function must be locked */
	struct mmap_hash_entry *entry = uaddr_to_mmap_entry(cur_process, (void*)masked_uaddr);
	if(entry == NULL){
		/* Process has just deleted this entry meaning that it was
		   not necessary to keep it. */
		return true;
	}

	struct fd_hash_entry *fd_entry = fd_to_fd_hash_entry(entry->fd);

	/* The file should never be closed as long as there is a
	   mmapping to it */
	ASSERT(fd_entry != NULL);

	off_t offset = masked_uaddr - entry->begin_addr;


	/* If this is the last page only read the appropriate number of bytes*/
	off_t write_bytes = (entry->end_addr - masked_uaddr) == PGSIZE  ?
			(entry->begin_addr + entry->length_of_file) - masked_uaddr : PGSIZE;

	/* because this frame is pinned we know we can write from the
	   kernel virtual address without worrying about getting
	   kicked off*/
	if(kaddr == NULL){
	  PANIC("kaddr is null when should never be null masked_uaddr is %p\n", (void *)masked_uaddr );
	}

	off_t amount_read = file_write_at(fd_entry->open_file, kaddr, write_bytes, offset);

	if(amount_read < write_bytes){
		PANIC("Error reading file in MMAP\n");
	}

	lock_release(&cur_process->mmap_table_lock);
	/* Clear this page so that it can be used, and set this PTE
	   back to on demand status*/
	ASSERT(pagedir_setup_demand_page(pd, (void*)masked_uaddr, PTE_MMAP,
			masked_uaddr, true));

	return true;
}

/* Converts a user address to a mmap_hash_entry, or NULL
   Will look through the hash table to see if uaddr
   is between the bounds of the given mmaped region*/
struct mmap_hash_entry *uaddr_to_mmap_entry(struct process *cur, void *uaddr){
	struct hash_iterator i;
	struct hash_elem *e;
	hash_first (&i, &cur->mmap_table);
	while((e = hash_next(&i)) != NULL){
		struct mmap_hash_entry *test =
				hash_entry(e, struct mmap_hash_entry, elem);
		if((uint32_t)uaddr < test->end_addr &&
				(uint32_t)uaddr >= test->begin_addr){
			return test;
		}
	}
	return NULL;
}

/* Given the map id looks up the mmap_hash_entry using the
   mid as the key. And either returns the entry or it will
   return NULL*/
struct mmap_hash_entry *mapid_to_hash_entry(mapid_t mid){
	struct process *process = thread_current()->process;
	struct mmap_hash_entry key;
	key.mmap_id = mid;
	struct hash_elem *map_hash_elem = hash_find(&process->mmap_table, &key.elem);
	if(map_hash_elem == NULL){
		return NULL;
	}
	return hash_entry(map_hash_elem, struct mmap_hash_entry, elem);
}

/* mmap hash function */
unsigned mmap_hash_func(const struct hash_elem *a, void *aux UNUSED){
	mapid_t mapid = hash_entry(a, struct mmap_hash_entry, elem)->mmap_id;
	return hash_bytes(&mapid, (sizeof(mapid_t)));
}

/* mmap hash compare function */
bool mmap_hash_compare (const struct hash_elem *a,
		const struct hash_elem *b, void *aux UNUSED){
	ASSERT(a != NULL);
	ASSERT(b != NULL);
	return (hash_entry(a, struct mmap_hash_entry, elem)->mmap_id <
			hash_entry(b, struct mmap_hash_entry, elem)->mmap_id);
}

/* call all destructor for hash_destroy */
void mmap_hash_entry_destroy (struct hash_elem *e, void *aux UNUSED){
	/*File close needs to be called here */
	struct mmap_hash_entry *entry = hash_entry(e, struct mmap_hash_entry, elem);
	mmap_save_all(entry);
	pagedir_clear_pages(thread_current()->pagedir,
			(uint32_t*)entry->begin_addr, entry->num_pages);
	free(hash_entry(e, struct mmap_hash_entry, elem));
}
