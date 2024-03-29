#include "userprog/process.h"
#include <debug.h>
#include <inttypes.h>
#include <round.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "userprog/gdt.h"
#include "userprog/tss.h"
#include "userprog/syscall.h"
#include "filesys/directory.h"
#include "filesys/file.h"
#include "filesys/filesys.h"
#include "threads/flags.h"
#include "threads/init.h"
#include "threads/interrupt.h"
#include "threads/palloc.h"
#include "threads/thread.h"
#include "threads/mmu.h"
#include "threads/vaddr.h"
#include "threads/malloc.h"
#include "intrinsic.h"
#ifdef VM
#include "vm/vm.h"
#include "vm/file.h"
#endif

static void process_cleanup (void);
static bool load (const char *file_name, struct intr_frame *if_);
static void initd (void *f_name);
static void __do_fork (void *);

extern struct semaphore file_access;

/*helper functions for managing child process exit status*/
static struct child_pipe init_process; //static variable for initial process
struct child_pipe *get_pipe_by_tid(struct thread *t,tid_t tid){
	struct thread *cur=t;
	struct list_elem *p;
	struct child_pipe *pipe;
	if(list_begin(&cur->child_list)==NULL){
		return NULL;
	}
	if(list_empty(&cur->child_list)){
		return NULL;
	}
	p=list_front(&cur->child_list);
	while(p != list_end(&cur->child_list)){
		pipe=list_entry(p, struct child_pipe, elem);
		if(pipe->tid==tid){
			return pipe;
		}
		p=list_next(p);
	}
	pipe=list_entry(p, struct child_pipe, elem);
	if(pipe->tid==tid){
		return pipe;
	}
	return NULL;
}

static struct child_pipe *allocate_pipe(void){
	return (struct child_pipe *)malloc(sizeof(struct child_pipe));
}

static void free_pipe(struct child_pipe * child){
	return free(child);
}
struct fd_cont *get_cont_by_fd(struct thread *t,int fd){//get fd_cont with fd from target thread.
	struct thread *cur=t;
	struct list_elem *p,*fdp;
	struct fd_cont *cont;
	if(list_begin(&cur->fd_list)==NULL){
		return NULL;
	}
	if(list_empty(&cur->fd_list)){
		return NULL;
	}
	p=list_front(&cur->fd_list);			//traverse thread's fd_list
	while(p !=list_end(&cur->fd_list)){
		cont=list_entry(p,struct fd_cont,elem);
		fdp=list_front(&cont->fdl);
		while(fdp != list_end(&cont->fdl)){
			if(list_entry(fdp,struct fd_list, elem)->fd==fd){
				return cont;
			}
			fdp=list_next(fdp);
		}
		p=list_next(p);
	}
	return NULL;
}

/* General process initializer for initd and other process. */
static void
process_init (void) {
	struct thread *current = thread_current ();
	current->num_fd=2;
	list_init(&current->child_list);
	list_init(&current->fd_list);
}

/* Starts the first userland program, called "initd", loaded from FILE_NAME.
 * The new thread may be scheduled (and may even exit)
 * before process_create_initd() returns. Returns the initd's
 * thread id, or TID_ERROR if the thread cannot be created.
 * Notice that THIS SHOULD BE CALLED ONCE. */
tid_t
process_create_initd (const char *file_name) {
	char *fn_copy;
	char thread_name[16];
	char *cp;
	tid_t tid;

	/* Make a copy of FILE_NAME.
	 * Otherwise there's a race between the caller and load(). */
	fn_copy = palloc_get_page (0);
	if (fn_copy == NULL)
		return TID_ERROR;
	strlcpy (fn_copy, file_name, PGSIZE);
	cp = strchr(fn_copy,' ');
	if(cp!=NULL) *cp='\0';
	strlcpy(thread_name,fn_copy,16);
	if(cp!=NULL) *cp=' ';

	/* Create a new thread to execute FILE_NAME. */
	tid = thread_create (thread_name, PRI_DEFAULT, initd, fn_copy);
	if (tid == TID_ERROR)
		palloc_free_page (fn_copy);
	return tid;
}

/* A thread function that launches first user process. */
static void
initd (void *f_name) {
#ifdef VM
	supplemental_page_table_init (&thread_current ()->spt);
#endif
	sema_init(&init_process.sema,0);
	init_process.tid=thread_current()->tid;
	init_process.exit_status=-1;
	thread_current()->parent_pipe = &init_process.elem;
	process_init ();
	
	thread_current()->cur_dir = dir_open_root();

	//allocate fd 0, 1 to STDIN, STDOUT
	struct fd_cont *cont=(struct fd_cont *)malloc(sizeof(struct fd_cont));
	if(cont==NULL) PANIC("Fail to launch initd\n");
	struct fd_list *fdl=(struct fd_list *)malloc(sizeof(struct fd_list));
	if(fdl==NULL){
		free(cont);
	       	PANIC("Fail to launch initd\n");
	}

	list_init(&cont->fdl);			//allocate STDIN
	fdl->fd=0;
	list_push_back(&cont->fdl,&fdl->elem);
	cont->file=NULL;
	cont->std=false;
	list_push_back(&thread_current()->fd_list,&cont->elem);
	
	cont=(struct fd_cont *)malloc(sizeof(struct fd_cont));
	fdl=(struct fd_list *)malloc(sizeof(struct fd_list));
	if(cont==NULL || fdl==NULL){
		if(cont != NULL) free(cont);
		if(fdl != NULL) free(fdl);
		cont=list_entry(list_pop_front(&thread_current()->fd_list),struct fd_cont,elem);
		fdl=list_entry(list_pop_front(&cont->fdl),struct fd_list,elem);
		free(fdl);
		free(cont);
	}
	list_init(&cont->fdl);			//allocate STDOUT
	fdl->fd=1;
	list_push_back(&cont->fdl,&fdl->elem);
	cont->file=NULL;
	cont->std=true;
	list_push_back(&thread_current()->fd_list,&cont->elem);
	if (process_exec (f_name) < 0)
		PANIC("Fail to launch initd\n");
	NOT_REACHED ();
}

/* Clones the current process as `name`. Returns the new process's thread id, or
 * TID_ERROR if the thread cannot be created. */
tid_t
process_fork (const char *name, struct intr_frame *if_ UNUSED) {
	/* Clone current thread to new thread.*/
	tid_t tid;
	struct thread *cur=thread_current();
	struct child_pipe *newpipe=allocate_pipe();	//make new child_pipe element, init,
	sema_init(&newpipe->sema,0);			
	newpipe->exit_status=-1;
	newpipe->tid=0;
	list_push_back(&cur->child_list,&newpipe->elem);

	cur->parent_if=if_;
	tid = thread_create (name,
			PRI_DEFAULT, __do_fork, thread_current ());

	sema_down(&newpipe->sema);
	if(newpipe->tid==0){
		list_remove(&newpipe->elem);
		free_pipe(newpipe);
		return -1;
	}else{
		return tid;
	}

}

#ifndef VM
/* Duplicate the parent's address space by passing this function to the
 * pml4_for_each. This is only for the project 2. */
static bool
duplicate_pte (uint64_t *pte, void *va, void *aux) {
	struct thread *current = thread_current ();
	struct thread *parent = (struct thread *) aux;
	void *parent_page;
	void *newpage;
	bool writable;

	/* 1. TODO: If the parent_page is kernel page, then return immediately. */
	if(is_kern_pte(pte))
		return true;
	
	/* 2. Resolve VA from the parent's page map level 4. */
	parent_page = pml4_get_page (parent->pml4, va);

	/* 3. TODO: Allocate new PAL_USER page for the child and set result to
	 *    TODO: NEWPAGE. */
	newpage=palloc_get_page(PAL_USER);

	/* 4. TODO: Duplicate parent's page to the new page and
	 *    TODO: check whether parent's page is writable or not (set WRITABLE
	 *    TODO: according to the result). */
	memcpy(newpage,parent_page,PGSIZE);
	writable=is_writable(pml4e_walk(parent->pml4,va,false));	
	

	/* 5. Add new page to child's page table at address VA with WRITABLE
	 *    permission. */
	if (!pml4_set_page (current->pml4, va, newpage, writable)) {
		/* 6. TODO: if fail to insert page, do error handling. */
		palloc_free_page(newpage);
		return false;
	}
	return true;
}
#endif

/* A thread function that copies parent's execution context.
 * Hint) parent->tf does not hold the userland context of the process.
 *       That is, you are required to pass second argument of process_fork to
 *       this function. */
static void
__do_fork (void *aux) {
	struct intr_frame if_;
	struct thread *parent = (struct thread *) aux;
	struct thread *current = thread_current ();
	/* TODO: somehow pass the parent_if. (i.e. process_fork()'s if_) */
	struct intr_frame *parent_if=parent->parent_if;
	bool succ = true;

	/* 1. Read the cpu context to local stack. */
	memcpy (&if_, parent_if, sizeof (struct intr_frame));

	/* 2. Duplicate PT */
	current->pml4 = pml4_create();
	if (current->pml4 == NULL)
		goto error;
	process_activate (current);
#ifdef VM
	supplemental_page_table_init (&current->spt);
	if (!supplemental_page_table_copy (&current->spt, &parent->spt))
		goto error;
#else
	if (!pml4_for_each (parent->pml4, duplicate_pte, parent))
		goto error;

#endif

	/* TODO: Your code goes here.
	 * TODO: Hint) To duplicate the file object, use `file_duplicate`
	 * TODO:       in include/filesys/file.h. Note that parent should not return
	 * TODO:       from the fork() until this function successfully duplicates
	 * TODO:       the resources of parent.*/

	if_.R.rax=0;		//return value of child should be 0
	current->cur_dir = dir_reopen(parent->cur_dir);
	
	struct child_pipe *pipe=list_entry(list_back(&parent->child_list),struct child_pipe,elem);
	ASSERT(pipe->tid==0);
	
	process_init ();
	
	struct fd_cont *new_fd, *parent_fd;

	if(!list_empty(&parent->fd_list)){
		struct list_elem *pfd_elem=list_front(&parent->fd_list);
		do{
			parent_fd = list_entry(pfd_elem,struct fd_cont,elem);
			new_fd=(struct fd_cont *)malloc(sizeof(struct fd_cont));
			if(new_fd==NULL){	//if malloc fails, free all resources(fd_cont, fd_list)
				goto free_res;
			}
			
			list_init(&new_fd->fdl);//initialize fd_list and add first fd to ita

			struct list_elem *parent_fde=list_front(&parent_fd->fdl);
			struct fd_list *parent_fdl, *fdl;
			do{
				parent_fdl=list_entry(parent_fde,struct fd_list,elem);
				fdl = (struct fd_list *)malloc(sizeof(struct fd_list));
				
				if(fdl==NULL) goto free_res;

				fdl->fd=parent_fdl->fd;
				list_push_back(&new_fd->fdl, &fdl->elem);
				parent_fde=list_next(parent_fde);
			}while(parent_fde != list_end(&parent_fd->fdl));
			
			if(parent_fd->file==NULL){
				new_fd->file=NULL;
				new_fd->std=parent_fd->std;
			}else{
				new_fd->file = file_duplicate(parent_fd->file);
			}
			list_push_back(&current->fd_list,&new_fd->elem);
			pfd_elem=list_next(pfd_elem);	
		}while(pfd_elem!=list_end(&parent->fd_list));
	}
	current->num_fd=parent->num_fd;
	

	pipe->tid=current->tid;
	current->parent_pipe=&pipe->elem;

	sema_up(&pipe->sema);
	/* Finally, switch to the newly created process. */
	if (succ)
		do_iret (&if_);
error:
	sema_up(&pipe->sema);
	thread_exit ();

free_res:
	while(!list_empty(&current->fd_list)){
		new_fd=list_entry(list_pop_front(&current->fd_list),struct fd_cont,elem);//get already allocated fd_cont
		while(!list_empty(&new_fd->fdl)){//free fd_list in fd_cont
			free(list_entry(list_pop_front(&new_fd->fdl),struct fd_list,elem));
		}
		file_close(new_fd->file);
		free(new_fd);
	}
	goto error;
}

/* Switch the current execution context to the f_name.
 * Returns -1 on fail. */
int
process_exec (void *f_name) {
	char *file_name=f_name;
	bool success;


	/* We cannot use the intr_frame in the thread structure.
	 * This is because when current thread rescheduled,
	 * it stores the execution information to the member. */
	struct intr_frame _if;
	_if.ds = _if.es = _if.ss = SEL_UDSEG;
	_if.cs = SEL_UCSEG;
	_if.eflags = FLAG_IF | FLAG_MBS;
	
	/* We first kill the current context */
	process_cleanup ();
	/* And then load the binary */
	success = load (file_name, &_if);

	/* If load failed, quit. */
	palloc_free_page (file_name);
	if (!success)
		return -1;
	/* Start switched process. */
	do_iret (&_if);
	NOT_REACHED ();
}


/* Waits for thread TID to die and returns its exit status.  If
 * it was terminated by the kernel (i.e. killed due to an
 * exception), returns -1.  If TID is invalid or if it was not a
 * child of the calling process, or if process_wait() has already
 * been successfully called for the given TID, returns -1
 * immediately, without waiting.
 *
 * This function will be implemented in problem 2-2.  For now, it
 * does nothing. */
int
process_wait (tid_t child_tid) {
	/* XXX: Hint) The pintos exit if process_wait (initd), we recommend you
	 * XXX:       to add infinite loop here before
	 * XXX:       implementing the process_wait. */
	struct child_pipe * child_info=get_pipe_by_tid(thread_current(),child_tid);
	int exit_status;
	if(child_info !=NULL){
		ASSERT(child_info->tid==child_tid);
		sema_down(&child_info->sema);
		exit_status = child_info->exit_status;
		list_remove(&child_info->elem);
		free_pipe(child_info);
		return exit_status;
	}else{
		if(child_tid==init_process.tid){
			sema_down(&init_process.sema);
			return init_process.exit_status;
		}
		return -1;
	}
}

/* Exit the process. This function is called by thread_exit (). */
void
process_exit (void) {
	struct thread *curr = thread_current ();
	/* TODO: Your code goes here.
	 * TODO: Implement process termination message (see
	 * TODO: project2/process_termination.html).
	 * TODO: We recommend you to implement process resource cleanup here. */
	process_cleanup ();
	if(list_begin(&curr->fd_list) != NULL){
		while(!list_empty(&curr->fd_list)){
			struct fd_cont *cont=list_entry(list_pop_front(&curr->fd_list), struct fd_cont,elem);
			while(!list_empty(&cont->fdl)){
				free(list_entry(list_pop_front(&cont->fdl),struct fd_list,elem));
			}
			sema_down(&file_access);
			file_close(cont->file);
			sema_up(&file_access);
			free(cont);
		}
	}
	if(list_begin(&curr->child_list) != NULL){
		while(!list_empty(&curr->child_list)){
			struct child_pipe *pipe=list_entry(list_pop_front(&curr->child_list),struct child_pipe,elem);
			free(pipe);
		}
	}
	if(curr->exec_file != NULL){
		sema_down(&file_access);
		file_close(curr->exec_file);
		sema_up(&file_access);
	}
	if(curr->parent_pipe != NULL){
		struct child_pipe *pipe=list_entry(curr->parent_pipe,struct child_pipe, elem);
		sema_up(&pipe->sema);
		printf("%s: exit(%d)\n",curr->name, pipe->exit_status);
	}
}

/* Free the current process's resources. */
static void
process_cleanup (void) {
	struct thread *curr = thread_current ();

#ifdef VM
	supplemental_page_table_kill (&curr->spt);
#endif

	uint64_t *pml4;
	/* Destroy the current process's page directory and switch back
	 * to the kernel-only page directory. */
	pml4 = curr->pml4;
	if (pml4 != NULL) {
		/* Correct ordering here is crucial.  We must set
		 * cur->pagedir to NULL before switching page directories,
		 * so that a timer interrupt can't switch back to the
		 * process page directory.  We must activate the base page
		 * directory before destroying the process's page
		 * directory, or our active page directory will be one
		 * that's been freed (and cleared). */
		curr->pml4 = NULL;
		pml4_activate (NULL);
		pml4_destroy (pml4);
	}
}

/* Sets up the CPU for running user code in the nest thread.
 * This function is called on every context switch. */
void
process_activate (struct thread *next) {
	/* Activate thread's page tables. */
	pml4_activate (next->pml4);

	/* Set thread's kernel stack for use in processing interrupts. */
	tss_update (next);
}

/* We load ELF binaries.  The following definitions are taken
 * from the ELF specification, [ELF1], more-or-less verbatim.  */

/* ELF types.  See [ELF1] 1-2. */
#define EI_NIDENT 16

#define PT_NULL    0            /* Ignore. */
#define PT_LOAD    1            /* Loadable segment. */
#define PT_DYNAMIC 2            /* Dynamic linking info. */
#define PT_INTERP  3            /* Name of dynamic loader. */
#define PT_NOTE    4            /* Auxiliary info. */
#define PT_SHLIB   5            /* Reserved. */
#define PT_PHDR    6            /* Program header table. */
#define PT_STACK   0x6474e551   /* Stack segment. */

#define PF_X 1          /* Executable. */
#define PF_W 2          /* Writable. */
#define PF_R 4          /* Readable. */

/* Executable header.  See [ELF1] 1-4 to 1-8.
 * This appears at the very beginning of an ELF binary. */
struct ELF64_hdr {
	unsigned char e_ident[EI_NIDENT];
	uint16_t e_type;
	uint16_t e_machine;
	uint32_t e_version;
	uint64_t e_entry;
	uint64_t e_phoff;
	uint64_t e_shoff;
	uint32_t e_flags;
	uint16_t e_ehsize;
	uint16_t e_phentsize;
	uint16_t e_phnum;
	uint16_t e_shentsize;
	uint16_t e_shnum;
	uint16_t e_shstrndx;
};

struct ELF64_PHDR {
	uint32_t p_type;
	uint32_t p_flags;
	uint64_t p_offset;
	uint64_t p_vaddr;
	uint64_t p_paddr;
	uint64_t p_filesz;
	uint64_t p_memsz;
	uint64_t p_align;
};

/* Abbreviations */
#define ELF ELF64_hdr
#define Phdr ELF64_PHDR

static bool setup_stack (struct intr_frame *if_);
static bool validate_segment (const struct Phdr *, struct file *);
static bool load_segment (struct file *file, off_t ofs, uint8_t *upage,
		uint32_t read_bytes, uint32_t zero_bytes,
		bool writable);

//push arguments into rsp in _f except argv[0].
static int push_args_to_stack(char **tokenizer, struct intr_frame * _f){
	struct thread *t=thread_current();
	int arglen;
	int argcnt;
	uint64_t argvptr=0; //for pushing address of each string
	
	ASSERT(tokenizer != NULL);
	char *arg=strtok_r(NULL," ",tokenizer);

	if(arg==NULL){
		//word_align, push argv[argc] null pointer
		if((_f->rsp & 7) != 0)
			_f->rsp = _f->rsp & ~(uint64_t)(7); //word aligning
		_f->rsp-=8;	//argv[argc] part
		return 0;
	}else{
		//push string
		arglen=strnlen(arg,PGSIZE);
		_f->rsp-=arglen+1;
		argvptr=_f->rsp;
		strlcpy((char *)(pml4_get_page(t->pml4,(void *)_f->rsp)),arg,arglen+1);

		//recursive call
		argcnt = push_args_to_stack(tokenizer,_f);

		//pushing address of string
		_f->rsp -= 8;
		*(uint64_t *)(pml4_get_page(t->pml4,(void *)_f->rsp))=argvptr;
		
		return argcnt+1;
	}
}

/* Loads an ELF executable from FILE_NAME into the current thread.
 * Stores the executable's entry point into *RIP
 * and its initial stack pointer into *RSP.
 * Returns true if successful, false otherwise. */
static bool
load (const char *file_name, struct intr_frame *if_) {
	struct thread *t = thread_current ();
	struct ELF ehdr;
	struct file *file = NULL;
	off_t file_ofs;
	bool success = false;
	uint64_t argptr;
	int i;
	char *argv;
	int argvlen;
	int argcnt;
	char *tokenizer;
	char file_tokens[129];

	strlcpy(file_tokens,file_name,strlen(file_name)+1);

	/* Allocate and activate page directory. */
	t->pml4 = pml4_create ();
	if (t->pml4 == NULL)
		goto done;
	process_activate (thread_current ());
	

	/* Open executable file. */
	argv=strtok_r(file_tokens," ",&tokenizer);
	sema_down(&file_access);
	file = filesys_open (argv);
	sema_up(&file_access);
	if (file == NULL) {
		printf ("load: %s: open failed\n", file_name);
		goto done;
	}
	/* Read and verify executable header. */
	if (file_read (file, &ehdr, sizeof ehdr) != sizeof ehdr
			|| memcmp (ehdr.e_ident, "\177ELF\2\1\1", 7)
			|| ehdr.e_type != 2
			|| ehdr.e_machine != 0x3E // amd64
			|| ehdr.e_version != 1
			|| ehdr.e_phentsize != sizeof (struct Phdr)
			|| ehdr.e_phnum > 1024) {
		printf ("load: %s: error loading executable\n", file_name);
		goto done;
	}

	/* Read program headers. */
	file_ofs = ehdr.e_phoff;
	for (i = 0; i < ehdr.e_phnum; i++) {
		struct Phdr phdr;

		if (file_ofs < 0 || file_ofs > file_length (file))
			goto done;
		file_seek (file, file_ofs);

		if (file_read (file, &phdr, sizeof phdr) != sizeof phdr)
			goto done;
		file_ofs += sizeof phdr;
		switch (phdr.p_type) {
			case PT_NULL:
			case PT_NOTE:
			case PT_PHDR:
			case PT_STACK:
			default:
				/* Ignore this segment. */
				break;
			case PT_DYNAMIC:
			case PT_INTERP:
			case PT_SHLIB:
				goto done;
			case PT_LOAD:
				if (validate_segment (&phdr, file)) {
					bool writable = (phdr.p_flags & PF_W) != 0;
					uint64_t file_page = phdr.p_offset & ~PGMASK;
					uint64_t mem_page = phdr.p_vaddr & ~PGMASK;
					uint64_t page_offset = phdr.p_vaddr & PGMASK;
					uint32_t read_bytes, zero_bytes;
					if (phdr.p_filesz > 0) {
						/* Normal segment.
						 * Read initial part from disk and zero the rest. */
						read_bytes = page_offset + phdr.p_filesz;
						zero_bytes = (ROUND_UP (page_offset + phdr.p_memsz, PGSIZE)
								- read_bytes);
					} else {
						/* Entirely zero.
						 * Don't read anything from disk. */
						read_bytes = 0;
						zero_bytes = ROUND_UP (page_offset + phdr.p_memsz, PGSIZE);
					}
					if (!load_segment (file, file_page, (void *) mem_page,
								read_bytes, zero_bytes, writable))
						goto done;
				}
				else
					goto done;
				break;
		}
	}

	/* Set up stack. */
	if (!setup_stack (if_))
		goto done;

	/* Start address. */
	if_->rip = ehdr.e_entry;
	
	/* TODO: Your code goes here.
	 * TODO: Implement argument passing (see project2/argument_passing.html). */

	//get length of argv[0] to calcualte rsp address
	argvlen=strnlen(argv,PGSIZE);
	if_->rsp-=argvlen+1;
	argptr=if_->rsp;

	//push string into stack, and call recursive function p_a_t_s to handle other arguments
	strlcpy((pml4_get_page(t->pml4,(void *)if_->rsp)),argv,argvlen+1);
	argcnt=push_args_to_stack(&tokenizer,if_)+1; //+1 is for argv[0]
	if_->rsp-=8;
	
	//push address of argv[0] into stack
	*(uint64_t *)(pml4_get_page(t->pml4,(void *)if_->rsp)) = argptr;

	if_->R.rdi=argcnt;//first parameter argc
	if_->R.rsi=if_->rsp;//second parameter argv[0]
	if_->rsp -=8;  //return address

	success = true;


	//hex_dump(if_->rsp,(void *)(pml4_get_page(t->pml4,(void *)if_->rsp)),256,false);

done:
	/* We arrive here whether the load is successful or not. */
	if(success){
		t->exec_file = file;
		file_deny_write(file);
	}else{
		file_close (file);
	}
	return success;
}


/* Checks whether PHDR describes a valid, loadable segment in
 * FILE and returns true if so, false otherwise. */
static bool
validate_segment (const struct Phdr *phdr, struct file *file) {
	/* p_offset and p_vaddr must have the same page offset. */
	if ((phdr->p_offset & PGMASK) != (phdr->p_vaddr & PGMASK))
		return false;

	/* p_offset must point within FILE. */
	if (phdr->p_offset > (uint64_t) file_length (file))
		return false;

	/* p_memsz must be at least as big as p_filesz. */
	if (phdr->p_memsz < phdr->p_filesz)
		return false;

	/* The segment must not be empty. */
	if (phdr->p_memsz == 0)
		return false;

	/* The virtual memory region must both start and end within the
	   user address space range. */
	if (!is_user_vaddr ((void *) phdr->p_vaddr))
		return false;
	if (!is_user_vaddr ((void *) (phdr->p_vaddr + phdr->p_memsz)))
		return false;

	/* The region cannot "wrap around" across the kernel virtual
	   address space. */
	if (phdr->p_vaddr + phdr->p_memsz < phdr->p_vaddr)
		return false;

	/* Disallow mapping page 0.
	   Not only is it a bad idea to map page 0, but if we allowed
	   it then user code that passed a null pointer to system calls
	   could quite likely panic the kernel by way of null pointer
	   assertions in memcpy(), etc. */
	if (phdr->p_vaddr < PGSIZE)
		return false;

	/* It's okay. */
	return true;
}

#ifndef VM
/* Codes of this block will be ONLY USED DURING project 2.
 * If you want to implement the function for whole project 2, implement it
 * outside of #ifndef macro. */

/* load() helpers. */
static bool install_page (void *upage, void *kpage, bool writable);

/* Loads a segment starting at offset OFS in FILE at address
 * UPAGE.  In total, READ_BYTES + ZERO_BYTES bytes of virtual
 * memory are initialized, as follows:
 *
 * - READ_BYTES bytes at UPAGE must be read from FILE
 * starting at offset OFS.
 *
 * - ZERO_BYTES bytes at UPAGE + READ_BYTES must be zeroed.
 *
 * The pages initialized by this function must be writable by the
 * user process if WRITABLE is true, read-only otherwise.
 *
 * Return true if successful, false if a memory allocation error
 * or disk read error occurs. */
static bool
load_segment (struct file *file, off_t ofs, uint8_t *upage,
		uint32_t read_bytes, uint32_t zero_bytes, bool writable) {
	ASSERT ((read_bytes + zero_bytes) % PGSIZE == 0);
	ASSERT (pg_ofs (upage) == 0);
	ASSERT (ofs % PGSIZE == 0);

	file_seek (file, ofs);
	while (read_bytes > 0 || zero_bytes > 0) {
		/* Do calculate how to fill this page.
		 * We will read PAGE_READ_BYTES bytes from FILE
		 * and zero the final PAGE_ZERO_BYTES bytes. */
		size_t page_read_bytes = read_bytes < PGSIZE ? read_bytes : PGSIZE;
		size_t page_zero_bytes = PGSIZE - page_read_bytes;

		/* Get a page of memory. */
		uint8_t *kpage = palloc_get_page (PAL_USER);
		if (kpage == NULL)
			return false;

		/* Load this page. */
		if (file_read (file, kpage, page_read_bytes) != (int) page_read_bytes) {
			palloc_free_page (kpage);
			return false;
		}
		memset (kpage + page_read_bytes, 0, page_zero_bytes);

		/* Add the page to the process's address space. */
		if (!install_page (upage, kpage, writable)) {
			printf("fail\n");
			palloc_free_page (kpage);
			return false;
		}

		/* Advance. */
		read_bytes -= page_read_bytes;
		zero_bytes -= page_zero_bytes;
		upage += PGSIZE;
	}
	return true;
}

/* Create a minimal stack by mapping a zeroed page at the USER_STACK */
static bool
setup_stack (struct intr_frame *if_) {
	uint8_t *kpage;
	bool success = false;

	kpage = palloc_get_page (PAL_USER | PAL_ZERO);
	if (kpage != NULL) {
		success = install_page (((uint8_t *) USER_STACK) - PGSIZE, kpage, true);
		if (success)
			if_->rsp = USER_STACK;
		else
			palloc_free_page (kpage);
	}
	return success;
}

/* Adds a mapping from user virtual address UPAGE to kernel
 * virtual address KPAGE to the page table.
 * If WRITABLE is true, the user process may modify the page;
 * otherwise, it is read-only.
 * UPAGE must not already be mapped.
 * KPAGE should probably be a page obtained from the user pool
 * with palloc_get_page().
 * Returns true on success, false if UPAGE is already mapped or
 * if memory allocation fails. */
static bool
install_page (void *upage, void *kpage, bool writable) {
	struct thread *t = thread_current ();

	/* Verify that there's not already a page at that virtual
	 * address, then map our page there. */
	return (pml4_get_page (t->pml4, upage) == NULL
			&& pml4_set_page (t->pml4, upage, kpage, writable));
}
#else
/* From here, codes will be used after project 3.
 * If you want to implement the function for only project 2, implement it on the
 * upper block. */


static bool
lazy_load_segment (struct page *page, void *aux) {
	/* TODO: Load the segment from the file */
	/* TODO: This called when the first page fault occurs on address VA. */
	/* TODO: VA is available when calling this function. */
	struct file_page *fi = (struct file_page *)aux;
	size_t page_read_bytes = fi->page_read_bytes;
	size_t page_zero_bytes = PGSIZE - page_read_bytes;
	uintptr_t kpage = page->frame->kva;
	
	file_read_at (fi->file, kpage, page_read_bytes,fi->ofs);
	/*if (file_read_at (fi->file, kpage, page_read_bytes,fi->ofs) != (int) page_read_bytes) {
	//	palloc_free_page (kpage);
		printf("!!error\n\n");
		free(fi);
		return false;
	}*/
	memset (kpage + page_read_bytes, 0, page_zero_bytes);
	free(fi);
	return true;
}

/* Loads a segment starting at offset OFS in FILE at address
 * UPAGE.  In total, READ_BYTES + ZERO_BYTES bytes of virtual
 * memory are initialized, as follows:
 *
 * - READ_BYTES bytes at UPAGE must be read from FILE
 * starting at offset OFS.
 *
 * - ZERO_BYTES bytes at UPAGE + READ_BYTES must be zeroed.
 *
 * The pages initialized by this function must be writable by the
 * user process if WRITABLE is true, read-only otherwise.
 *
 * Return true if successful, false if a memory allocation error
 * or disk read error occurs. */
static bool
load_segment (struct file *file, off_t ofs, uint8_t *upage,
		uint32_t read_bytes, uint32_t zero_bytes, bool writable) {
	ASSERT ((read_bytes + zero_bytes) % PGSIZE == 0);
	ASSERT (pg_ofs (upage) == 0);
	ASSERT (ofs % PGSIZE == 0);
	uint32_t offset = 0;
	while (read_bytes > 0 || zero_bytes > 0) {
		/* Do calculate how to fill this page.
		 * We will read PAGE_READ_BYTES bytes from FILE
		 * and zero the final PAGE_ZERO_BYTES bytes. */
		size_t page_read_bytes = read_bytes < PGSIZE ? read_bytes : PGSIZE;
		size_t page_zero_bytes = PGSIZE - page_read_bytes;

		/* TODO: Set up aux to pass information to the lazy_load_segment. */
		struct file_page *fi = (struct file_page *)malloc(sizeof(struct file_page));
		fi->file = file;
		fi->ofs = (ofs+offset*PGSIZE);
		fi->page_read_bytes = page_read_bytes;
		offset++;
		void *aux = fi;
		if (!vm_alloc_page_with_initializer (VM_ANON, upage,
					writable, lazy_load_segment, aux))
			return false;

		/* Advance. */
		read_bytes -= page_read_bytes;
		zero_bytes -= page_zero_bytes;
		upage += PGSIZE;
	}
	return true;
}


/* Create a PAGE of stack at the USER_STACK. Return true on success. */
static bool
setup_stack (struct intr_frame *if_) {
	bool success = false;
	void *stack_bottom = (void *) (((uint8_t *) USER_STACK) - PGSIZE);

	/* TODO: Map the stack on stack_bottom and claim the page immediately.
	 * TODO: If success, set the rsp accordingly.
	 * TODO: You should mark the page is stack. */
	/* TODO: Your code goes here */
	if(!vm_alloc_page_with_initializer(VM_ANON | VM_MARKER_0, stack_bottom, true, NULL, NULL))
		return false;
	success = vm_claim_page(stack_bottom);
	if(success){
		memset(stack_bottom,0,PGSIZE);
		if_->rsp = USER_STACK;
	}
	return success;
}
#endif /* VM */
