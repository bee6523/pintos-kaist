/* anon.c: Implementation of page for non-disk image (a.k.a. anonymous page). */

#include "vm/vm.h"
#include "devices/disk.h"
#include "lib/kernel/bitmap.h"
#include "threads/synch.h"
#include "threads/vaddr.h"

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
	return true;
}

/* Swap in the page by read contents from the swap disk. */
static bool
anon_swap_in (struct page *page, void *kva) {
	struct anon_page *anon_page = &page->anon;
	size_t i;
	if(anon_page->swap_idx == -1 || anon_page->swap_idx % 8)/* swap index -1 means no swap disk allocatded.
								   swap index should be multiple of 8
								   because PGSIZE is 4096byte, sector size is 512
								   */
		return false;
	for(i=0;i<8;i++){
		disk_read(swap_disk, anon_page->swap_idx + i, kva + i*DISK_SECTOR_SIZE);
	}
	sema_down(&st_access);
	bitmap_set_multiple(swap_table, anon_page->swap_idx, 8, false);
	sema_up(&st_access);
	anon_page->swap_idx = -1;	//now no allocation
	return true;
}

/* Swap out the page by writing contents to the swap disk. */
static bool
anon_swap_out (struct page *page) {
	struct anon_page *anon_page = &page->anon;
	size_t i;
	sema_down(&st_access);
	anon_page->swap_idx = bitmap_scan_and_flip(swap_table,0,8,false);
	sema_up(&st_access);
	if(anon_page->swap_idx == BITMAP_ERROR)
		PANIC("no available space at swap disk");
	for(i=0;i<8;i++){
		disk_write(swap_disk,anon_page->swap_idx + i,page->va + i*DISK_SECTOR_SIZE);
	}
	//printf("haha %d", *(int *)page->va);
	return true;
}

/* Destroy the anonymous page. PAGE will be freed by the caller. */
static void
anon_destroy (struct page *page) {
	struct anon_page *anon_page = &page->anon;
	if(anon_page->swap_idx != -1){	//if data is in swap disk, free them by changing swap table
		sema_down(&st_access);
		bitmap_set_multiple(swap_table, anon_page->swap_idx, 8, false);
		sema_up(&st_access);
	}
}
