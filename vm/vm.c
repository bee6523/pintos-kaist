/* vm.c: Generic interface for virtual memory objects. */

#include "threads/malloc.h"
#include "threads/vaddr.h"
#include "vm/vm.h"
#include "vm/inspect.h"
#include "vm/anon.h"
#include "vm/file.h"

struct frame_table ft;
struct semaphore ft_access;

/* hash helper functions */

static unsigned spt_hash_func(const struct hash_elem *p_, void *aux UNUSED){
	const struct page *p = hash_entry(p_, struct page, elem);
	return hash_bytes(&p->va, sizeof(p->va));
}
static bool spt_less_func(const struct hash_elem *a_, const struct hash_elem *b_, void * aux UNUSED){
	const struct page *a = hash_entry(a_, struct page, elem);
	const struct page *b = hash_entry(b_, struct page, elem);

	return a->va < b->va;
}
static unsigned ft_hash_func(const struct hash_elem *p_, void *aux UNUSED){
	const struct frame *p = hash_entry(p_, struct frame, elem);
	return hash_bytes(&p->kva, sizeof(p->kva));
}
static bool ft_less_func(const struct hash_elem *a_, const struct hash_elem *b_, void * aux UNUSED){
	const struct frame *a = hash_entry(a_, struct frame, elem);
	const struct frame *b = hash_entry(b_, struct frame, elem);

	return a->kva < b->kva;
}

/* Initializes the virtual memory subsystem by invoking each subsystem's
 * intialize codes. */
void
vm_init (void) {
	vm_anon_init ();
	vm_file_init ();
#ifdef EFILESYS  /* For project 4 */
	pagecache_init ();
#endif
	register_inspect_intr ();
	/* DO NOT MODIFY UPPER LINES. */
	/* TODO: Your code goes here. */

	
	hash_init(&ft.ft_hash, ft_hash_func, ft_less_func,NULL);
	hash_first(&ft.hand, &ft.ft_hash);
	sema_init(&ft_access,1);
}


/* Get the type of the page. This function is useful if you want to know the
 * type of the page after it will be initialized.
 * This function is fully implemented now. */
enum vm_type
page_get_type (struct page *page) {
	int ty = VM_TYPE (page->operations->type);
	switch (ty) {
		case VM_UNINIT:
			return VM_TYPE (page->uninit.type);
		default:
			return ty;
	}
}

/* Helpers */
static struct frame *vm_get_victim (void);
static bool vm_do_claim_page (struct page *page);
static struct frame *vm_evict_frame (void);

/* Create the pending page object with initializer. If you want to create a
 * page, do not create it directly and make it through this function or
 * `vm_alloc_page`. */
bool
vm_alloc_page_with_initializer (enum vm_type type, void *upage, bool writable,
		vm_initializer *init, void *aux) {

	ASSERT (VM_TYPE(type) != VM_UNINIT)

	struct supplemental_page_table *spt = &thread_current ()->spt;
	struct page *page = NULL;
	

	/* Check wheter the upage is already occupied or not. */
	if (spt_find_page (spt, upage) == NULL) {
		/* TODO: Create the page, fetch the initialier according to the VM type,
		 * TODO: and then create "uninit" page struct by calling uninit_new. You
		 * TODO: should modify the field after calling the uninit_new. */
		page = (struct page *)malloc(sizeof(struct page));
		if(page == NULL) goto err;
		switch(VM_TYPE(type)){
			case VM_ANON:
				uninit_new(page,upage,init,type,aux,anon_initializer);
				break;
			case VM_FILE:
				uninit_new(page,upage,init,type,aux,file_map_initializer);
				break;
			default:
				PANIC("wrong vm_type");
		}
		page->pml4 = thread_current()->pml4;
		page->type = type;
		page->writable = writable;
		/* TODO: Insert the page into the spt. */
		if(spt_insert_page(spt, page))
			return true;
	}
err:
	return false;
}

/* Find VA from spt and return page. On error, return NULL. */
struct page *
spt_find_page (struct supplemental_page_table *spt, void *va) {
	struct page *page = NULL;
	/* TODO: Fill this function. */
	struct page temp;
	struct hash_elem *e;

	temp.va = (void *)((uintptr_t)va & (~PGMASK));	//aligning va to PGSIZE
	e = hash_find(&spt->spt_hash, &temp.elem);
	if(e != NULL)
		page = hash_entry(e, struct page, elem);
	return page;
}

/* Insert PAGE into spt with validation. */
bool
spt_insert_page (struct supplemental_page_table *spt,
		struct page *page) {
	int succ = false;
	/* TODO: Fill this function. */
	if(hash_insert(&spt->spt_hash, &page->elem) == NULL){
		succ=true;
	}

	return succ;
}

void
spt_remove_page (struct supplemental_page_table *spt, struct page *page) {
	hash_delete(&spt->spt_hash, &page->elem);
	vm_dealloc_page (page);
	return true;
}

/* helper function for vm_get_victim. 
   check if one of pages refering frame has accessed frame */
static bool is_frame_accessed(const struct frame *frame){
	uint64_t *pml4 = frame->page->pml4;
	return (pml4_is_accessed(pml4,frame->kva) || pml4_is_accessed(pml4,frame->page->va));
}
static void set_frame_accessed_zero(const struct frame *frame){
	uint64_t *pml4 = frame->page->pml4;
	pml4_set_accessed(pml4,frame->kva,false);
	pml4_set_accessed(pml4,frame->page->va,false);
}

/* Get the struct frame, that will be evicted. */
static struct frame *
vm_get_victim (void) {
	//struct frame *victim = NULL;
	 /* TODO: The policy for eviction is up to you. */
	/* policy : clock algorithm */
	struct hash_elem *e;
	struct frame * candidate;
	hash_first(&ft.hand, &ft.ft_hash);
	while(true){
		e = hash_next(&ft.hand);
		if(e==NULL){	//if last element of hash
			hash_first(&ft.hand, &ft.ft_hash);	//goto first element and find again
			continue;
		}
		candidate = hash_entry(e, struct frame, elem);
	/*	if(candidate->page == NULL){
	//	printf("evicted frame: aa%x\n\n",candidate->kva);
	//		set_frame_accessed_zero(candidate);
			victim = candidate;
			break;
		}*/
		if(is_frame_accessed(candidate)){
			set_frame_accessed_zero(candidate);
		}else
			return candidate;
	}
}

/* Evict one page and return the corresponding frame.
 * Return NULL on error.*/
static struct frame *
vm_evict_frame (void) {
	sema_down(&ft_access);
	struct frame *victim = vm_get_victim ();
	sema_up(&ft_access);
	/* TODO: swap out the victim and return the evicted frame. */
	if(victim->page != NULL){
		swap_out(victim->page);
		pml4_clear_page(victim->page->pml4,victim->page->va);
		victim->page->frame = NULL;
		victim->page = NULL;
	}
	return victim;
}

/* palloc() and get frame. If there is no available page, evict the page
 * and return it. This always return valid address. That is, if the user pool
 * memory is full, this function evicts the frame to get the available memory
 * space.*/
static struct frame *
vm_get_frame (void) {
	struct frame *frame = NULL;
	/* TODO: Fill this function. */
	void * ppage = palloc_get_page(PAL_USER);
	struct hash_elem *chk;
	if(ppage == NULL){
		frame = vm_evict_frame();
	}else{
		frame = (struct frame *)malloc(sizeof(struct frame));
		if(frame == NULL){
			PANIC("todo?");
		}
		frame->kva = ppage;
		frame->page = NULL;

		sema_down(&ft_access);
		chk = hash_insert(&ft.ft_hash, &frame->elem);
		sema_up(&ft_access);

		if(chk != NULL){
			free(frame);
			frame = hash_entry(chk, struct frame, elem);
		}	
	}

	ASSERT (frame != NULL);
	ASSERT (frame->page == NULL);
	return frame;
}

/* Growing the stack. */
static void
vm_stack_growth (void *addr) {
	bool success = false;
	void *stack_addr = (void *) ((uintptr_t)addr & ~(PGMASK));
	if(!vm_alloc_page_with_initializer(VM_ANON | VM_MARKER_0, stack_addr, true, NULL, NULL))
		return false;
	success = vm_claim_page(stack_addr);
	if(success){
		memset(stack_addr,0,PGSIZE);
	}
}

/* Handle the fault on write_protected page */
static bool
vm_handle_wp (struct page *page UNUSED) {
	return true;
}

/* Return true on success */
bool
vm_try_handle_fault (struct intr_frame *f UNUSED, void *addr,
		bool user UNUSED, bool write UNUSED, bool not_present UNUSED) {
	struct supplemental_page_table *spt = &thread_current ()->spt;
	struct page *page = NULL;
	/* TODO: Validate the fault */
	/* TODO: Your code goes here */
	uintptr_t rsp;
	if(user && is_kernel_vaddr(addr)) return false;
	page = spt_find_page(spt,addr);
	if(page==NULL){
		if(user){		//user case rsp setting
			rsp = f->rsp;
		}else			//kernel case
			rsp = thread_current()->trsp;
	
/*		if(write){
			printf("\n\naddr %x, %d %d, rsp %x or %x, np %d,  USER_STACK %x.\n\n",addr, user, write, rsp,thread_current()->trsp, not_present, USER_STACK-256*PGSIZE);
		}*/
		if(write && ((uintptr_t)addr >= rsp-8)){
			if(addr < ((uint8_t *)USER_STACK - 256*PGSIZE) || addr > USER_STACK){
				return false;
			}
			vm_stack_growth(addr);
			return true;
		}else{
			return false;
	//		thread_exit();		//error case
		}
	}else if(page->frame != NULL){
		//copy on write case.
		if(page->writable && write){
			//should check copy on write
			return vm_handle_wp(page);
		}
		return false; //otherwise should not happen
	}
	
	return vm_do_claim_page (page);
}

/* Free the page.
 * DO NOT MODIFY THIS FUNCTION. */
void
vm_dealloc_page (struct page *page) {
	destroy (page);
	free (page);
}

/* Claim the page that allocate on VA. */
bool
vm_claim_page (void *va) {
	struct page *page = NULL;
	/* TODO: Fill this function */
	struct hash_elem *e;
	struct page p;
	p.va = va;
	e = hash_find(&thread_current()->spt.spt_hash,&p.elem);
	if(e==NULL) return false;
	else{
		page = hash_entry(e, struct page, elem);
		return vm_do_claim_page (page);
	}
}

/* Claim the PAGE and set up the mmu. */
static bool
vm_do_claim_page (struct page *page) {
	struct frame *frame = vm_get_frame ();
	/* Set links */
	frame->page = page;
	page->frame = frame;
	/* TODO: Insert page table entry to map page's VA to frame's PA. */
	if(!pml4_set_page(page->pml4,page->va,frame->kva,page->writable))
		return false;
	return swap_in (page, frame->kva);
}


/* Initialize new supplemental page table */
void
supplemental_page_table_init (struct supplemental_page_table *spt) {
	hash_init(&spt->spt_hash, spt_hash_func, spt_less_func,NULL);
}

/* Copy supplemental page table from src to dst */
bool
supplemental_page_table_copy (struct supplemental_page_table *dst,
		struct supplemental_page_table *src) {
	struct hash_iterator i;
	struct page * page;
	void * upage;
	bool writable;
	enum vm_type type;
	hash_first(&i, &src->spt_hash);
	while(hash_next(&i)){
		struct page *spte = hash_entry(hash_cur(&i),struct page, elem);
		upage = spte->va;
		type = spte->type;
		writable = spte->writable;
		/* Check wheter the upage is already occupied or not. */

		if (spt_find_page (dst, upage) == NULL) {
			page = (struct page *)malloc(sizeof(struct page));
			if(page == NULL) goto err;
			switch(VM_TYPE(type)){
				case VM_ANON:
					uninit_new(page,upage,NULL,type,NULL,anon_initializer);
					break;
				case VM_FILE:
					uninit_new(page,upage,NULL,type,NULL,file_map_initializer);
					break;
				default:
					PANIC("wrong vm_type");
			}
			page->pml4 = thread_current()->pml4;
			page->type = type;
			page->writable = writable;
			if(!spt_insert_page(dst, page))
				goto err;
			if(!vm_do_claim_page(page))
				goto err;
			if(spte->frame == NULL && !vm_do_claim_page(spte))
				goto err;
			memcpy(page->frame->kva,spte->frame->kva,PGSIZE);
		}
	}
	return true;
err:	
	printf("\n\nerror occured\n\n");
	return false;
}

void
free_hash_element(struct hash_elem *element, void *aux UNUSED){
	struct page *spte = hash_entry(element, struct page, elem);
//	if(spte->frame){
//		sema_down(&ft_access);
//		hash_delete(&ft, &spte->frame->elem);
//		sema_up(&ft_access);
//		free(spte->frame);
//	}
	vm_dealloc_page(spte);
}

/* Free the resource hold by the supplemental page table */
void
supplemental_page_table_kill (struct supplemental_page_table *spt) {
	/* TODO: Destroy all the supplemental_page_table hold by thread and
	 * TODO: writeback all the modified contents to the storage. */
	hash_clear(&spt->spt_hash, free_hash_element);
}
