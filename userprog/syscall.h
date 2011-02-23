#ifndef USERPROG_SYSCALL_H
#define USERPROG_SYSCALL_H
#include "filesys/file.h"
#include "threads/interrupt.h"
#include "userprog/process.h"
void syscall_init (void);

/* Also called from exception.c */
void system_exit (struct intr_frame *f, int status );

bool mmap_read_in(void *faulting_addr);
bool mmap_read_out(uint32_t *pd, void *uaddr);

#endif /* userprog/syscall.h */
