/* file.c: Implementation of memory mapped file object (mmaped object). */

#include "vm/vm.h"
#include "threads/vaddr.h"
#include "threads/malloc.h"

static bool file_map_swap_in (struct page *page, void *kva);
static bool file_map_swap_out (struct page *page);
static void file_map_destroy (struct page *page);

extern struct semaphore file_access;

/* DO NOT MODIFY this struct */
static const struct page_operations file_ops = {
	.swap_in = file_map_swap_in,
	.swap_out = file_map_swap_out,
	.destroy = file_map_destroy,
	.type = VM_FILE,
};

/* The initializer of file vm */
void
vm_file_init (void) {
}

/* Initialize the file mapped page */
bool
file_map_initializer (struct page *page, enum vm_type type, void *kva) {
	/* Set up the handler */
	page->operations = &file_ops;

	struct file_page *file_page = &page->file;
}

/* check if dirty */
static bool is_dirty(const struct page *page){
	uint64_t *pml4 = page->pml4;
	if(pml4_is_dirty(pml4,page->va))// || pml4_is_dirty(pml4, page->frame->kva))
		return true;
	else return false;
}

/* Swap in the page by read contents from the file. */
static bool
file_map_swap_in (struct page *page, void *kva) {
	struct file_page *file_page UNUSED = &page->file;
	sema_down(&file_access);
	file_read_at(file_page->file, kva, file_page->page_read_bytes, file_page->ofs);
	sema_up(&file_access);
	memset(kva + file_page->page_read_bytes, 0, PGSIZE-file_page->page_read_bytes);
	pml4_set_accessed(page->pml4,kva,false);
	pml4_set_dirty(page->pml4,kva,false);
	return true;
}

/* Swap out the page by writeback contents to the file. */
static bool
file_map_swap_out (struct page *page) {
	struct file_page *file_page UNUSED = &page->file;
	if(is_dirty(page)){
		sema_down(&file_access);
		file_write_at(file_page->file, page->va,file_page->page_read_bytes, file_page->ofs);
		sema_up(&file_access);
	}
	return true;
}

/* Destory the file mapped page. PAGE will be freed by the caller. */
static void
file_map_destroy (struct page *page) {
	struct file_page *file_page = &page->file;
	if(is_dirty(page)){
		sema_down(&file_access);
		file_write_at(file_page->file, page->va,file_page->page_read_bytes, file_page->ofs);
		sema_up(&file_access);
	}
	(*file_page->mmap_count)--;
	if((*file_page->mmap_count)==0){
		sema_down(&file_access);
		file_close(file_page->file);
		sema_up(&file_access);
		free(file_page->mmap_count);
	}
}


/* lazy_mapping function for filemap page */
static bool
lazy_map_segment (struct page *page, void *aux) {
	/* TODO: Load the segment from the file */
	/* TODO: This called when the first page fault occurs on address VA. */
	/* TODO: VA is available when calling this function. */
	struct file_page *fi = (struct file_page *)aux;
	size_t page_read_bytes = fi->page_read_bytes;
	size_t page_zero_bytes = PGSIZE - page_read_bytes;
	uintptr_t kpage = page->frame->kva;
	size_t check = file_length(fi->file)-fi->ofs;
	if( check < page_read_bytes){
	       	page_read_bytes = check;
		page_zero_bytes = PGSIZE - page_read_bytes;
	}
	sema_down(&file_access);
	file_read_at(fi->file,kpage,page_read_bytes,fi->ofs);
/*	if (file_read_at (fi->file, kpage, page_read_bytes, fi->ofs) != (int)page_read_bytes) {
		printf("\nerror reading. check:%d, pg:%d\n",check,page_read_bytes);
		sema_up(&file_access);
		free(fi);
		return false;
	}*/
	sema_up(&file_access);
	memset(kpage + page_read_bytes, 0, page_zero_bytes);
	page->file.file= fi->file;
	page->file.ofs = fi->ofs;
	page->file.page_read_bytes = fi->page_read_bytes;
	page->file.mmap_count = fi->mmap_count;
	free(fi);
	return true;
}
/* Do the mmap */
void *
do_mmap (void *addr, size_t length, int writable,
		struct file *file, off_t offset) {	
	ASSERT (pg_ofs (addr) == 0);
	ASSERT (offset % PGSIZE == 0);
	uint32_t pgnum = 0;
	size_t *cnt = (size_t *)malloc(sizeof(size_t));
	struct file *reopen_file;
	enum vm_type type;
	void * ret = addr;
	if(file_length(file) < offset)
		return NULL;


	sema_down(&file_access);
	reopen_file = file_reopen(file);
	sema_up(&file_access);
	*cnt = 0;
	while (length > 0) {
		/* Do calculate how to fill this page.
		 * We will read PAGE_READ_BYTES bytes from FILE
		 * and zero the final PAGE_ZERO_BYTES bytes. */
		size_t page_read_bytes = length < PGSIZE ? length : PGSIZE;
		size_t page_zero_bytes = PGSIZE - page_read_bytes;

		/* TODO: Set up aux to pass information to the lazy_load_segment. */
		struct file_page *fi = (struct file_page *)malloc(sizeof(struct file_page));
		fi->file = reopen_file;
		fi->ofs = offset + pgnum*PGSIZE;
		fi->page_read_bytes = page_read_bytes;
		fi->mmap_count = cnt;
		(*cnt)++;

		if(page_read_bytes==length)
			type = VM_FILE | F_LAST_PAGE;
		else type = VM_FILE;
		pgnum++;

		void *aux = fi;
		if (!vm_alloc_page_with_initializer (type, addr,
					writable, lazy_map_segment, aux))
			return NULL;

		/* Advance. */
		length -= page_read_bytes;
		addr += PGSIZE;
	}
	return ret;
}

/* Do the munmap */
void
do_munmap (void *addr) {
	struct supplemental_page_table *spt = &thread_current()->spt;
	struct page * fp = spt_find_page(spt, addr);
	bool left = true;
	size_t pgnum = 1;
	if(VM_TYPE(fp->type) == VM_FILE){
		while(left){
			if(fp->type & F_LAST_PAGE){	//if this is last file-mapped page
				left = false;
			}
			destroy(fp);
			if(fp->frame){
				fp->frame->page = NULL;
				pml4_clear_page(fp->pml4,fp->va);
			}
			spt_remove_page(spt, fp);
			fp = spt_find_page(spt, addr + pgnum*PGSIZE);
			pgnum++;
		}
	}
}			
