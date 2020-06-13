/* anon.c: Implementation of page for non-disk image (a.k.a. anonymous page). */

#include "vm/vm.h"
#include "devices/disk.h"
#include "lib/kernel/bitmap.h"
#include "lib/string.h"
#include "threads/synch.h"
#include "threads/vaddr.h"
#include "threads/mmu.h"

/* DO NOT MODIFY BELOW LINE */
static struct disk *swap_disk;
static bool anon_swap_in (struct page *page, void *kva);
static bool anon_swap_out (struct page *page);
static void anon_destroy (struct page *page);

/* DO NOT MODIFY this struct */
static const struct page_operations anon_ops = {
	.swap_in = anon_swap_in,
	.swap_out = anon_swap_out,
	.destroy = anon_destroy,
	.type = VM_ANON,
};
//each page has 8 sectors
#define NUM_SECTOR 8
struct bitmap * swap_table;
struct semaphore st_access;
/* Initialize the data for anonymous pages */
void
vm_anon_init (void) {
	/* TODO: Set up the swap_disk. */
	swap_disk = disk_get(1,1);
	swap_table = bitmap_create(disk_size(swap_disk));
	sema_init(&st_access,1);
}

/* Initialize the file mapping */
bool
anon_initializer (struct page *page, enum vm_type type, void *kva) {
	/* Set up the handler */
	page->operations = &anon_ops;
	
	struct anon_page *anon_page = &page->anon;
	anon_page->swap_idx = -1;
	anon_page->swap_status = bitmap_create(NUM_SECTOR);
	return true;
}

/* Swap in the page by read contents from the swap disk. */
static bool
anon_swap_in (struct page *page, void *kva) {
	struct anon_page *anon_page = &page->anon;
	size_t i;
	if(anon_page->swap_idx == -1){
		//memset(kva,0,PGSIZE);		//if no swap disk allocated, can be considered as zero page. or maybe not
		//return true;
		return false;
	}
	/* 
	   swap index -1 means no swap disk allocatded.
	  swap index should be multiple of 8
	  because PGSIZE is 4096byte, sector size is 512
	
	if(anon_page->swap_idx % NUM_SECTOR)								   
		return false;
	*/
	for(i=0;i<8;i++){
		if(bitmap_test(anon_page->swap_status,i))
			disk_read(swap_disk, anon_page->swap_idx + i, kva + i*DISK_SECTOR_SIZE);
		else
			memset(kva + i*DISK_SECTOR_SIZE,0, DISK_SECTOR_SIZE);
	}
	bitmap_set_multiple(swap_table, anon_page->swap_idx, 8, false);
	
	pml4_set_accessed(page->pml4,kva,false);
	pml4_set_dirty(page->pml4,kva,false);
	
	anon_page->swap_idx = -1;	//now no allocation
	return true;
}
/* check if page is zero page */
static bool
is_zeros(void * addr, size_t size){
	unsigned char *va = addr;
	ASSERT(va != NULL);
	while(size-- > 0){
		if( *va != 0)
			return false;
		va++;
	}
	return true;
}


/* Swap out the page by writing contents to the swap disk. */
static bool
anon_swap_out (struct page *page) {
	struct anon_page *anon_page = &page->anon;
	size_t i;
	/*if(is_zeropage(page->va)){	//check if page is all zero bytes
		anon_page->swap_idx = -1;
		return true;
	}*/
	sema_down(&st_access);
	anon_page->swap_idx = bitmap_scan_and_flip(swap_table,0,8,false);
	sema_up(&st_access);
	if(anon_page->swap_idx == BITMAP_ERROR)
		PANIC("no available space at swap disk");
	if(pml4_is_dirty(page->pml4,page->va) || pml4_is_dirty(page->pml4,page->frame->kva)){	//if page is dirty, set swap status
		for(i=0;i<8;i++){
			if(!is_zeros(page->va+i*DISK_SECTOR_SIZE,DISK_SECTOR_SIZE)){
				disk_write(swap_disk,anon_page->swap_idx + i,page->va + i*DISK_SECTOR_SIZE);
				bitmap_mark(anon_page->swap_status,i);//set bit in swap_status
			}else
				bitmap_reset(anon_page->swap_status,i);//set bit in swap_status
		}		
	}else{
		for(i=0;i<8;i++){
			if(bitmap_test(anon_page->swap_status,i))
				disk_write(swap_disk,anon_page->swap_idx + i,page->va + i*DISK_SECTOR_SIZE);
		}
	}
	return true;
}

/* Destroy the anonymous page. PAGE will be freed by the caller. */
static void
anon_destroy (struct page *page) {
	struct anon_page *anon_page = &page->anon;
	if(anon_page->swap_idx != -1){	//if data is in swap disk, free them by changing swap table
		bitmap_set_multiple(swap_table, anon_page->swap_idx, 8, false);
	}
	bitmap_destroy(anon_page->swap_status);
}
