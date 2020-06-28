#ifndef FILESYS_PAGE_CACHE_H
#define FILESYS_PAGE_CACHE_H
#include "vm/vm.h"
#include "filesys/fat.h"

struct page;
enum vm_type;

struct page_cache {
	cluster_t cluster_idx;
	char cache_idx;	//-1 if not allocated
	void *kva;
	bool is_accessed;
	struct bitmap *swap_status;
};

void page_cache_init (void);
bool page_cache_initializer (struct page *page, enum vm_type type, void *kva);
#endif
