/* vm.c: Generic interface for virtual memory objects. */

#include "threads/malloc.h"
#include "threads/vaddr.h"
#include "vm/vm.h"
#include "vm/inspect.h"
#include "vm/anon.h"
#include "vm/file.h"

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
 * `vm_alloc_page`.
 * DO NOT MODIFY THIS FUNCTION. */
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
		switch(type&7){
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
	struct sup_pte temp;
	struct hash_elem *e;

	temp.addr = (void *)((uintptr_t)va & (~(PGSIZE-1)));	//aligning va to PGSIZE
	e = hash_find(&spt->spt_hash, &temp.elem);
	if(e != NULL)
		page = hash_entry(e, struct sup_pte, elem)->page;
	return page;
}

/* Insert PAGE into spt with validation. */
bool
spt_insert_page (struct supplemental_page_table *spt,
		struct page *page) {
	int succ = false;
	/* TODO: Fill this function. */
	struct sup_pte *spte = (struct sup_pte *)malloc(sizeof(struct sup_pte));
	if(spte != NULL){
		spte->page = page;
		spte->addr = page->va;
		if(hash_insert(&spt->spt_hash, &spte->elem) == NULL)
			succ=true;
	}

	return succ;
}

void
spt_remove_page (struct supplemental_page_table *spt, struct page *page) {
	vm_dealloc_page (page);
	return true;
}

/* Get the struct frame, that will be evicted. */
static struct frame *
vm_get_victim (void) {
	struct frame *victim = NULL;
	 /* TODO: The policy for eviction is up to you. */

	return victim;
}

/* Evict one page and return the corresponding frame.
 * Return NULL on error.*/
static struct frame *
vm_evict_frame (void) {
	struct frame *victim UNUSED = vm_get_victim ();
	/* TODO: swap out the victim and return the evicted frame. */

	return NULL;
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
	if(ppage == NULL){
		PANIC("todo");
	}
	frame = (struct frame *)malloc(sizeof(struct frame));
	if(frame == NULL){
		PANIC("todo?");
	}
	frame->kva = ppage;
	frame->page = NULL;
	
	ASSERT (frame != NULL);
	ASSERT (frame->page == NULL);
	return frame;
}

/* Growing the stack. */
static void
vm_stack_growth (void *addr) {
	bool success = false;
	void *stack_addr = (void *) ((uintptr_t)addr & ~(PGSIZE-1));
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
			printf("\n\naddr %x, %d %d, rsp %x or %x, USER_STACK %x.\n\n",addr, user, write, rsp,thread_current()->trsp, USER_STACK);
		}*/
		if(write && ((uintptr_t)addr >= rsp-8)){
			if(addr < ((uint8_t *)USER_STACK - 256*PGSIZE))
				return false;
			vm_stack_growth(addr);
			return true;
		}else{
			return false;
	//		thread_exit();		//error case
		}
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
	struct sup_pte p;
	p.addr = va;
	e = hash_find(&thread_current()->spt.spt_hash,&p.elem);
	if(e==NULL) return false;
	else{
		page = hash_entry(e, struct sup_pte, elem)->page;
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

static unsigned spt_hash_func(const struct hash_elem *p_, void *aux UNUSED){
	const struct sup_pte *p = hash_entry(p_, struct sup_pte, elem);
	return hash_bytes(&p->addr, sizeof(p->addr));
}
static bool spt_less_func(const struct hash_elem *a_, const struct hash_elem *b_, void * aux UNUSED){
	const struct sup_pte *a = hash_entry(a_, struct sup_pte, elem);
	const struct sup_pte *b = hash_entry(b_, struct sup_pte, elem);

	return a->addr < b->addr;
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
		struct sup_pte *spte = hash_entry(hash_cur(&i),struct sup_pte, elem);
		upage = spte->page->va;
		type = spte->page->type;
		writable = spte->page->writable;
		/* Check wheter the upage is already occupied or not. */

		if (spt_find_page (dst, upage) == NULL) {
			page = (struct page *)malloc(sizeof(struct page));
			if(page == NULL) goto err;
			switch(type&7){
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
			if(spte->page->frame == NULL && !vm_do_claim_page(spte->page))
				goto err;
			memcpy(page->frame->kva,spte->page->frame->kva,PGSIZE);
		}
	}
	return true;
err:	
	printf("\n\nerror occured\n\n");
	return false;
}

void
free_hash_element(struct hash_elem *element, void *aux UNUSED){
	struct sup_pte *spte = hash_entry(element, struct sup_pte, elem);
	destroy(spte->page);
	free(spte);
}

/* Free the resource hold by the supplemental page table */
void
supplemental_page_table_kill (struct supplemental_page_table *spt UNUSED) {
	/* TODO: Destroy all the supplemental_page_table hold by thread and
	 * TODO: writeback all the modified contents to the storage. */
	hash_clear(&spt->spt_hash, free_hash_element);
}
