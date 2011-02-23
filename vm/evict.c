#include "page.h"
#include <stdint.h>
#include <stdbool.h>
#include "threads/vaddr.h"
#include "threads/thread.h"
#include "userprog/process.h"
#include "vm/frame.h"
#include <debug.h>


static size_t evict_hand;
static site_t clear_hand;
static size_t threshold;

static void *relocate_page (struct frame_hash_entry *frame);



/* returns the key to the frame that is now available, use the entry
   to install this page into the pagedir of the evicting thread that
   is asking for memory.   

   
   MEMORY - If we try to evict a page, it's medium should say that it is
   in memory or it does not make sense that we are evicting it.

   SWAP - If we evict something in memory and it is dirty and not a mmaped
   file we put it on swap, swap_allocate takes care of the rest.

   STACK - If we evict a stack page that's clean, we can just get a new
   page from thin air.  If we evict a dirty stack page, we write it to swap.

   EXECUTABLE - cannot write to executable, if we evict this we do nothing.
   MMAP - If we evict an mmapped page we write it back to the file system.   */
void *evict_page(){
  
    struct bitmap *bmap = f_table->used_frames;
    
    while((evict_hand + threshold) % frame_table_size() < clear_hand){
        /* Our clear hand is still at least theshold bits in front of us */
        struct frame_hash_entry *frame = get_frame_at_position(evict_hand);
	ASSERT(frame != NULL);
	/*return the first page we find that has not been accesed */
	if(!pagedir_is_accesed(frame->current_page_dir,frame->page)){
	    /*  evict page from frame moving it to the appropriate
		location and return the kernel virtual address of the 
		physical page represented by the evict_hand in the bitmap*/
	    return relocate_page(frame);
	}
      
    }
    /* in this case we need to move both hands simultaneously until the
       evict_hand finds a !accessed page */

}

void evict_init(){
  threshold = 1; 
  evict_hand = 0;
  clear_hand = evict_hand + threshold;
}

static void *relocate_page (struct frame_hash_entry *frame){
    if(pagedir_is_dirty(frame->current_pagedir, frame->page)){
        medium_t medium = pagedir_get_medium(frame->current_pagedir,frame->page);
      
	ASSERT(medium != PTE_AVL_MEMORY);
      
	if(medium == PTE_AVL_STACK || medium == PTE_AVL_EXEC){
	  /* write to swap */
	}else if(medium == PTE_AVL_MMAP){
	  /* write back to disk */
	}else{
  	    PANIC("relocate_page called with dirty page of medium_t: %x", medium);
	}
    }else{
      if(medium == PTE_AVL_STACK){
	/* User has read a 0'd page they have not written to, delete the
	   page, return frame. */
      }else if(medium == PTE_AVL_EXEC){
	/* this one should just set up on demand page again */
      }else if(medium == PTE_AVL_MMAP){
	/* this should also set up an on demand page */
      }else{
	  PANIC("realocate_page called with clean page of medium_t: %x", medium);
      }
    }
    /* return the frame corresponding to evict_hand */
}

