/* page_cache.c: Implementation of Page Cache (Buffer Cache). */

#include "vm/vm.h"
#include "devices/disk.h"
#include "devices/timer.h"
#include "lib/kernel/bitmap.h"
#include "threads/vaddr.h"
#include "threads/synch.h"
#include "filesys/filesys.h"
#include "filesys/fat.h"

static bool page_cache_readahead (struct page *page, void *kva);
static bool page_cache_writeback (struct page *page);
static void page_cache_destroy (struct page *page);
static void page_cache_kworkerd (void *aux);
static void regular_writeback_worker (void *aux);

/* DO NOT MODIFY this struct */
static const struct page_operations page_cache_op = {
	.swap_in = page_cache_readahead,
	.swap_out = page_cache_writeback,
	.destroy = page_cache_destroy,
	.type = VM_PAGE_CACHE,
};

tid_t page_cache_workerd;
tid_t writeback_worker;
void * page_cache_kpage;
struct page *alloc_pages[8];//pages in cache

struct list swapin_queue;
struct lock cache_lock;
struct condition not_empty;
struct condition job_done;



/* The initializer of file vm */
void
page_cache_init (void) {
	/* TODO: Create a worker daemon for page cache with page_cache_kworkerd */
	page_cache_kpage = palloc_get_multiple(PAL_USER,8);
	
	list_init(&swapin_queue);
	lock_init(&cache_lock);
	cond_init(&not_empty);
	cond_init(&job_done);
	
	page_cache_workerd = thread_create("pcache_worker",PRI_DEFAULT,page_cache_kworkerd,NULL);
	writeback_worker = thread_create("writeback_worker",PRI_DEFAULT, regular_writeback_worker,NULL);
}

/* Initialize the page cache */
bool
page_cache_initializer (struct page *page, enum vm_type type, void *kva) {
	/* Set up the handler */
	struct page_cache *pcache = &page->page_cache;
	page->operations = &page_cache_op;
	pcache->cache_idx = -1;
	pcache->enqueued = false;
	pcache->swap_status = bitmap_create(8);
	return true;
}

/* Utilze the Swap in mechanism to implement readhead */
static bool
page_cache_readahead (struct page *page, void *kva) {
	/*
	lock_acquire(&cache_lock);
	list_push_front(&swapin_queue,&page->list_elem);
	while(page->page_cache.kva == NULL)		//wait until allocation
		cond_wait(&job_done, &cache_lock);

	//add next block to swapin queue

	lock_release(&cache_lock);
	*/
	struct page_cache *pcache = &page->page_cache;
	disk_sector_t sector = cluster_to_sector(page->va);

	for(int i=0;i<8;i++){
		if(sector+i < disk_size(filesys_disk))
			disk_read(filesys_disk,sector+i,pcache->kva+i*DISK_SECTOR_SIZE);
	}
	pcache->is_accessed = false;
	bitmap_set_all(pcache->swap_status,false);
	return true;
}

/* Utilze the Swap out mechanism to implement writeback */
static bool
page_cache_writeback (struct page *page) {
	struct page_cache *pcache = &page->page_cache;
	disk_sector_t sector = cluster_to_sector(page->va);
	for(int i=0;i<8;i++){
		if(bitmap_test(pcache->swap_status,i)){
			disk_write(filesys_disk, sector+i,pcache->kva + i*DISK_SECTOR_SIZE);
			bitmap_set(pcache->swap_status,i,false);
		}
	}
	return true;
}

/* Destory the page_cache. */
static void
page_cache_destroy (struct page *page) {
	struct page_cache *pcache = &page->page_cache;
	lock_acquire(&cache_lock);
	if(pcache->cache_idx != -1){
		swap_out(page);
		alloc_pages[pcache->cache_idx]=NULL;
	}
	lock_release(&cache_lock);
	bitmap_destroy(pcache->swap_status);
}

/* Worker thread for page cache */
static void
page_cache_kworkerd (void *aux UNUSED) {
	struct list_elem *e;
	struct page *page;
	struct page_cache *pcache;
	int i=0;
	while(1){	//infinite loop
		lock_acquire(&cache_lock);
		while(list_empty(&swapin_queue)){
			cond_wait(&not_empty,&cache_lock);
		}
		//swapin page.
		e = list_pop_front(&swapin_queue);
		page = list_entry(e, struct page,list_elem);
		pcache = &page->page_cache;
		pcache->enqueued = false;
		if(pcache->cache_idx != -1){
			cond_signal(&job_done, &cache_lock);
			lock_release(&cache_lock);
			continue;
		}

		while(alloc_pages[i] != NULL){		//eviction - clock algorithm
			if(alloc_pages[i]->page_cache.is_accessed){
				alloc_pages[i]->page_cache.is_accessed = false;
				i++;
				if(i>=8) i=0;
				continue;
			}else{
				lock_acquire(&alloc_pages[i]->pglock);
				swap_out(alloc_pages[i]);	//remove page from cache
				alloc_pages[i]->page_cache.kva = 0;
				alloc_pages[i]->page_cache.cache_idx = -1;
				lock_release(&alloc_pages[i]->pglock);
				alloc_pages[i]=NULL;
				break;
			}
		}
		alloc_pages[i] = page;
		pcache->cache_idx = i;
		pcache->kva = (void *)((char *)page_cache_kpage+(i*PGSIZE));
		pcache->is_accessed = true;
		i++;
		if(i>=8) i=0;
		swap_in(page,pcache->kva);//read data from disk to cache
		cond_broadcast(&job_done,&cache_lock);
		lock_release(&cache_lock);
	}
}
static void
regular_writeback_worker (void *aux UNUSED){
	struct page *page;
	int i=0;
	while(1){
		timer_sleep(100);
		lock_acquire(&cache_lock);
		for(i=0;i<8;i++){
			if(alloc_pages[i] != NULL){
				swap_out(alloc_pages[i]);
			}
		}
		lock_release(&cache_lock);
	}
}
