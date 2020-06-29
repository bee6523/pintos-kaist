#include "userprog/syscall.h"
#include <stdio.h>
#include <syscall-nr.h>
#include "threads/interrupt.h"
#include "threads/thread.h"
#include "threads/loader.h"
#include "userprog/gdt.h"
#include "threads/flags.h"
#include "threads/init.h"
#include "threads/vaddr.h"
#include "threads/synch.h"
#include "threads/palloc.h"
#include "threads/malloc.h"
#include "userprog/process.h"
#include "filesys/filesys.h"
#include "filesys/file.h"
#include "lib/string.h"
#include "intrinsic.h"
#include "vm/vm.h"
#include "vm/file.h"

void syscall_entry (void);
void syscall_handler (struct intr_frame *);

/* System call.
 *
 * Previously system call services was handled by the interrupt handler
 * (e.g. int 0x80 in linux). However, in x86-64, the manufacturer supplies
 * efficient path for requesting the system call, the `syscall` instruction.
 *
 * The syscall instruction works by reading the values from the the Model
 * Specific Register (MSR). For the details, see the manual. */

#define MSR_STAR 0xc0000081         /* Segment selector msr */
#define MSR_LSTAR 0xc0000082        /* Long mode SYSCALL target */
#define MSR_SYSCALL_MASK 0xc0000084 /* Mask for the eflags */

struct semaphore file_access;

void s_file_close(struct file *file){		//synchronized file closing provided for process_exit()
//	sema_down(&file_access);
	file_close(file);
//	sema_up(&file_access);
}
static bool isKernelAddrs(uint64_t uaddr, uint64_t size);
static void validateBuffer(uint64_t uaddr, uint64_t size);
static void validateAddress(uint64_t uaddr);
static int allocate_fd(void);
static struct fd_cont *allocate_fd_cont(void);
static void free_fd_cont(struct fd_cont *cont);

void
syscall_init (void) {
	write_msr(MSR_STAR, ((uint64_t)SEL_UCSEG - 0x10) << 48  |
			((uint64_t)SEL_KCSEG) << 32);
	write_msr(MSR_LSTAR, (uint64_t) syscall_entry);

	/* The interrupt service rountine should not serve any interrupts
	 * until the syscall_entry swaps the userland stack to the kernel
	 * mode stack. Therefore, we masked the FLAG_FL. */
	sema_init(&file_access,1);
	write_msr(MSR_SYSCALL_MASK,
			FLAG_IF | FLAG_TF | FLAG_DF | FLAG_IOPL | FLAG_AC | FLAG_NT);
}

/* The main system call interface */
void
syscall_handler (struct intr_frame *f UNUSED) {
	// TODO: Your implementation goes here.
	//printf ("system call %d!\n", f->R.rax);
	struct thread *cur=thread_current();
	struct fd_cont *container;
	struct list_elem *fdl;
	struct file * file;
	uint64_t callee_reg[6];
	uintptr_t callee_rsp;
	uint64_t ret;

	cur->trsp = f->rsp;
	switch(f->R.rax){
		case SYS_HALT:
			power_off();
			break;
		case SYS_EXIT:
			list_entry(cur->parent_pipe,struct child_pipe, elem)->exit_status=f->R.rdi;
			
			thread_exit();
			break;
		case SYS_FORK:
			validateAddress(f->R.rdi);
			callee_reg[0]=f->R.rbx;		//save callee saved registers
			callee_reg[1]=f->R.rbp;
			callee_reg[2]=f->R.r12;
			callee_reg[3]=f->R.r13;
			callee_reg[4]=f->R.r14;
			callee_reg[5]=f->R.r15;
			callee_rsp=f->rsp;

			ret=process_fork((char *)f->R.rdi,f);
			
			f->R.rax=ret;
			f->rsp=callee_rsp;		//restore callee saved registers
			f->R.rbx=callee_reg[0];
			f->R.rbp=callee_reg[1];
			f->R.r12=callee_reg[2];
			f->R.r13=callee_reg[3];
			f->R.r14=callee_reg[4];
			f->R.r15=callee_reg[5];
			break;
		case SYS_EXEC:
			validateAddress(f->R.rdi);
			char *fn_copy = palloc_get_page (PAL_USER);	//freeing fn_copy is in process_exec
			if (fn_copy == NULL)
				return TID_ERROR;
			strlcpy (fn_copy, (char *)f->R.rdi, PGSIZE);

			ret=process_exec(fn_copy);
			f->R.rax=ret;
			break;
		case SYS_WAIT:
			ret=process_wait(f->R.rdi);
			f->R.rax=ret;
			break;
		case SYS_CREATE:
			validateAddress(f->R.rdi);
			validateAddress(f->R.rdi+strlen((char *)f->R.rdi)+1);
			sema_down(&file_access);
			f->R.rax = filesys_create((char *)f->R.rdi,f->R.rsi);
			sema_up(&file_access);
			break;
		case SYS_REMOVE:
			validateAddress(f->R.rdi);
			sema_down(&file_access);
			f->R.rax = filesys_remove((char *)f->R.rdi);
			sema_up(&file_access);
			break;
		case SYS_OPEN:
			validateAddress(f->R.rdi);
			sema_down(&file_access);
			file = filesys_open((char *)f->R.rdi);
			sema_up(&file_access);
			if(file==NULL){
				f->R.rax=-1;
				break;
			}else{
				container = allocate_fd_cont();
				int fd = allocate_fd();
				struct fd_list *fdl = (struct fd_list *)malloc(sizeof(struct fd_list));
				if(fdl==NULL){
					sema_down(&file_access);
					file_close(file);
					sema_up(&file_access);
					free_fd_cont(container);
					f->R.rax=-1;
					break;
				}
				list_init(&container->fdl);
				fdl->fd=fd;
				list_push_back(&container->fdl,&fdl->elem);
				container->file=file;
				list_push_back(&cur->fd_list,&container->elem);
				f->R.rax=fd;
			}
			break;
		case SYS_FILESIZE:
			container=get_cont_by_fd(cur,f->R.rdi);
			if(container==NULL || container->file == NULL){
				f->R.rax=0;
				break;
			}
			sema_down(&file_access);
			f->R.rax= file_length(container->file);
			sema_up(&file_access);
			break;
		case SYS_READ:
			validateBuffer(f->R.rsi,f->R.rdx);
			container=get_cont_by_fd(cur,f->R.rdi);
			if(container==NULL){
				f->R.rax=-1;
				break;
			}
			if(container->file==NULL){	//it means this fd is for STDIO
				if(!container->std){	//check if it is STDIN
					for(int i=0;i<f->R.rdx;i++){
						*((char *)(f->R.rsi)+i)=input_getc();
					}
					f->R.rax=f->R.rdx;
				}else f->R.rax=-1;	//error if STDOUT
			}else{
				struct page * pg;
				if((pg = spt_find_page(&cur->spt,f->R.rsi)) == NULL){
					if(!(f->R.rsi > f->rsp-8 && f->R.rsi < USER_STACK))	//if not stack grow case, exit
						thread_exit();
				}else if(!pg->writable){	//write to writable
					thread_exit();
				}
				sema_down(&file_access);
				f->R.rax = file_read(container->file, f->R.rsi, f->R.rdx);
				sema_up(&file_access);
			}
			break;
		case SYS_WRITE:
			validateBuffer(f->R.rsi,f->R.rdx);
	/*
			if(f->R.rdi==1){
				putbuf(f->R.rsi,f->R.rdx);
			}else{
	*/
			container=get_cont_by_fd(cur,f->R.rdi);
			if(container==NULL){
				f->R.rax=-1;
				break;
			}
			if(container->file==NULL){
				if(container->std){
					putbuf(f->R.rsi,f->R.rdx);
					f->R.rax=f->R.rdx;
				}else f->R.rax=-1;
			}else{
				sema_down(&file_access);
				f->R.rax = file_write(container->file, f->R.rsi,f->R.rdx);
				sema_up(&file_access);
			}
			break;
		case SYS_SEEK:
			container=get_cont_by_fd(cur,f->R.rdi);
			if(container==NULL || container->file==NULL)
				break;
			sema_down(&file_access);
			file_seek(container->file,f->R.rsi);
			sema_up(&file_access);
			break;
		case SYS_TELL:
			container=get_cont_by_fd(cur,f->R.rdi);
			if(container==NULL || container->file==NULL){
				f->R.rax=-1;
				break;
			}
			sema_down(&file_access);
			f->R.rax = file_tell(container->file);
			sema_up(&file_access);
			break;
		case SYS_CLOSE:
			container = get_cont_by_fd(cur,f->R.rdi);
			if(container==NULL)
				break;
			fdl=list_front(&container->fdl);
			while(list_entry(fdl,struct fd_list,elem)->fd != f->R.rdi)
				fdl=list_next(fdl);
			list_remove(fdl);
			free(list_entry(fdl,struct fd_list,elem));
			if(list_empty(&container->fdl)){
				sema_down(&file_access);
				file_close(container->file);
				sema_up(&file_access);
				list_remove(&container->elem);
				free_fd_cont(container);
			}
			break;
		case SYS_DUP2:
			container = get_cont_by_fd(cur,f->R.rdi);
			if(container==NULL){
				f->R.rax=-1;
				break;
			}
			if(f->R.rdi==f->R.rsi){
				f->R.rax=f->R.rsi;
				break;
			}
			struct fd_list *fde=(struct fd_list *)malloc(sizeof(struct fd_list));//allocate new fd_list entry
			if(fde==NULL){
				f->R.rax=-1;
				break;
			}

			struct fd_cont * cont2=get_cont_by_fd(cur,f->R.rsi);
			if(cont2!= NULL){		//when newfd was previously open, same as SYS_CLOSE
				fdl=list_front(&cont2->fdl);
				while(list_entry(fdl,struct fd_list, elem)->fd != f->R.rsi)
					fdl=list_next(fdl);
				list_remove(fdl);
				free(list_entry(fdl,struct fd_list,elem));
				if(list_empty(&cont2->fdl)){
					sema_down(&file_access);
					file_close(cont2->file);
					sema_up(&file_access);
					list_remove(&cont2->elem);
					free_fd_cont(cont2);
				}
			}

			fde->fd=f->R.rsi;
			list_push_back(&container->fdl,&fde->elem);	//add this fd to file description.

			f->R.rax=f->R.rsi;
			break;
		case SYS_MMAP:
			container=get_cont_by_fd(cur,f->R.r10);
			if(container==NULL){
				f->R.rax=NULL;
				break;
			}
			if(container->file==NULL || file_length(container->file)==0 || f->R.rdi==0 || isKernelAddrs(f->R.rdi,f->R.rsi) ||
				       	   f->R.rsi == 0 || f->R.r8 > PGSIZE){	//it means this fd is for STDIO
				f->R.rax = NULL;
				break;
			}else{
				if(f->R.rdi&(PGMASK) || f->R.r8&(PGMASK)){
					f->R.rax = NULL;
					break;
				}
				f->R.rax = do_mmap(f->R.rdi,f->R.rsi,f->R.rdx,container->file,f->R.r8);
			}

			break;
		case SYS_MUNMAP:
			validateAddress(f->R.rdi);
			do_munmap(f->R.rdi);
			break;

		default:
			thread_exit();
	}
//	do_iret(f);
}
static bool isKernelAddrs(uint64_t uaddr, uint64_t size){
	uint64_t i=0;
	for(i=0;i<size;i++){
		if(is_kernel_vaddr(uaddr+i))
			return true;
	}
	return false;
}

static void validateBuffer(uint64_t uaddr, uint64_t size){
	uint64_t i=0;
	//hex_dump(uaddr,(void *)(pml4_get_page(thread_current()->pml4,(void *)uaddr)),256,false);
	for(i=0;i<size;i++){
		validateAddress(uaddr+i);
	}

}
static void validateAddress(uint64_t uaddr){
	struct thread *t=thread_current();
	if(uaddr==0 ||is_kernel_vaddr(uaddr) || pml4e_walk(t->pml4,uaddr,0)==NULL)
	{
	//	printf("\nwhy? %x %d \n",uaddr, is_kernel_vaddr(uaddr));
		thread_exit();
	}
}
static int allocate_fd(void){
	struct thread *t=thread_current();
	return t->num_fd++;
}
static struct fd_cont *allocate_fd_cont(void){
	return (struct fd_cont *)malloc(sizeof(struct fd_cont));
}
static void free_fd_cont(struct fd_cont *cont){
	free(cont);
}
