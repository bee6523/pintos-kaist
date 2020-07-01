#ifndef FILESYS_PAGE_CACHE_H
#define FILESYS_PAGE_CACHE_H
#include "vm/vm.h"
#include "filesys/fat.h"

struct page;
enum vm_type;

struct page_cache {
	cluster_t cluster_idx;
	bool is_accessed;
	struct list_elem elem;
	struct bitmap *swap_status;
};

void page_cache_init (void);
void page_cache_close(void);
bool page_cache_initializer (struct page *page, enum vm_type type, void *kva);
struct page * page_cache_find(cluster_t clst);
struct page * pcache_evict_cache(void);
#endif
