#include "filesys/inode.h"
#include <list.h>
#include <debug.h>
#include <round.h>
#include <string.h>
#include <hash.h>
#include <bitmap.h>
#include "vm/vm.h"
#include "filesys/filesys.h"
#include "filesys/free-map.h"
#include "filesys/fat.h"
#include "filesys/page_cache.h"
#include "threads/malloc.h"

/* Identifies an inode. */
#define INODE_MAGIC 0x494e4f44

/* On-disk inode.
 * Must be exactly DISK_SECTOR_SIZE bytes long. */
struct inode_disk {
	cluster_t start;                /* First data cluster. */
	off_t length;                       /* File size in bytes. */
	unsigned magic;                     /* Magic number. */
	uint32_t unused[125];               /* Not used. */
};

/* Returns the number of sectors to allocate for an inode SIZE
 * bytes long. */
static inline size_t
bytes_to_clusters (off_t size) {
	return DIV_ROUND_UP (size, DISK_SECTOR_SIZE*SECTORS_PER_CLUSTER);
}

/* In-memory inode. */
struct inode {
	struct list_elem elem;              /* Element in inode list. */
	cluster_t cluster;		    /* sector number of disk location. */
	int open_cnt;                       /* Number of openers. */
	bool removed;                       /* True if deleted, false otherwise. */
	int deny_write_cnt;                 /* 0: writes ok, >0: deny writes. */
	struct inode_disk data;             /* Inode content. */
};

static unsigned inode_hash_func(const struct hash_elem *p_, void *aux UNUSED){
	const struct page *p = hash_entry(p_, struct page, elem);
	return hash_bytes(&p->page_cache.cluster_idx, sizeof(&p->page_cache.cluster_idx));
}
static bool inode_less_func(const struct hash_elem *a_, const struct hash_elem *b_, void * aux UNUSED){
	const struct page *a = hash_entry(a_, struct page, elem);
	const struct page *b = hash_entry(b_, struct page, elem);
	return a->page_cache.cluster_idx < b->page_cache.cluster_idx;
}

/* Returns the disk sector that contains byte offset POS within
 * INODE.
 * Returns -1 if INODE does not contain data for a byte at offset
 * POS. */
static cluster_t
byte_to_cluster (struct inode *inode, off_t pos, bool create) {
	ASSERT (inode != NULL);
	cluster_t clst = inode->data.start;
	ASSERT(clst != 0);
	cluster_t tmp;
	if (pos < inode->data.length || create){
		off_t i=pos/(DISK_SECTOR_SIZE*SECTORS_PER_CLUSTER);
		for(;i>0;i--){
			tmp = clst;
			clst = fat_get(clst);
			if(clst==EOChain){
				static char zeros[DISK_SECTOR_SIZE];
				clst = fat_create_chain(tmp);
				disk_write(filesys_disk, cluster_to_sector(clst),zeros);
				inode->data.length += DISK_SECTOR_SIZE;
			}
		}
		return clst;	
	}else
		return -1;
}

/* List of open inodes, so that opening a single inode twice
 * returns the same `struct inode'. */
static struct list open_inodes;
static struct hash open_inode_sectors;
extern struct list swapin_queue;
extern struct lock cache_lock;
extern struct condition not_empty;
extern struct condition job_done;

/* Initializes the inode module. */
void
inode_init (void) {
	list_init (&open_inodes);
	hash_init(&open_inode_sectors, inode_hash_func, inode_less_func,NULL);
}

/* Initializes an inode with LENGTH bytes of data and
 * writes the new inode to sector SECTOR on the file system
 * disk.
 * Returns true if successful.
 * Returns false if memory or disk allocation fails. */
bool
inode_create (cluster_t cluster, off_t length) {
	struct inode_disk *disk_inode = NULL;
	bool success = false;
	disk_sector_t sector = cluster_to_sector(cluster);

	ASSERT (length >= 0);
	/* If this assertion fails, the inode structure is not exactly
	 * one sector in size, and you should fix that. */
	ASSERT (sizeof *disk_inode == DISK_SECTOR_SIZE);

	disk_inode = calloc (1, sizeof *disk_inode);
	if (disk_inode != NULL) {
		size_t sectors = bytes_to_clusters (length);
		disk_inode->length = length;
		disk_inode->magic = INODE_MAGIC;
		if (free_fat_allocate (sectors, &disk_inode->start)) {
			disk_write (filesys_disk, sector, disk_inode);
			if (sectors > 0) {
				static char zeros[DISK_SECTOR_SIZE];
				cluster_t tmp;

				for (tmp = disk_inode->start; tmp != EOChain; tmp=fat_get(tmp)) 
					disk_write (filesys_disk, cluster_to_sector(tmp), zeros); 
			}
			success = true; 
		}
		free (disk_inode);
	}
	return success;
}

/* Reads an inode from SECTOR
 * and returns a `struct inode' that contains it.
 * Returns a null pointer if memory allocation fails. */
struct inode *
inode_open (cluster_t cluster) {
	struct list_elem *e;
	struct inode *inode;

	disk_sector_t sector = cluster_to_sector(cluster);

	/* Check whether this inode is already open. */
	for (e = list_begin (&open_inodes); e != list_end (&open_inodes);
			e = list_next (e)) {
		inode = list_entry (e, struct inode, elem);
		if (inode->cluster == cluster) {
			inode_reopen (inode);
			return inode; 
		}
	}

	/* Allocate memory. */
	inode = malloc (sizeof *inode);
	if (inode == NULL)
		return NULL;

	/* Initialize. */
	list_push_front (&open_inodes, &inode->elem);
	inode->cluster = cluster;
	inode->open_cnt = 1;
	inode->deny_write_cnt = 0;
	inode->removed = false;
	disk_read (filesys_disk, sector, &inode->data);
	return inode;
}

/* Reopens and returns INODE. */
struct inode *
inode_reopen (struct inode *inode) {
	if (inode != NULL){
		inode->open_cnt++;
	}
	return inode;
}

/* Returns INODE's inode number. */
disk_sector_t
inode_get_inumber (const struct inode *inode) {
	return inode->cluster;
}

/* Closes INODE and writes it to disk.
 * If this was the last reference to INODE, frees its memory.
 * If INODE was also a removed inode, frees its blocks. */
void
inode_close (struct inode *inode) {
	/* Ignore null pointer. */
	if (inode == NULL)
		return;

	/* Release resources if this was the last opener. */
	if (--inode->open_cnt == 0) {
		/* Remove from inode list and release lock. */
		list_remove (&inode->elem);

		/* Deallocate blocks if removed. */
		if (inode->removed) {
			fat_remove_chain (inode->cluster,0);
			fat_remove_chain (inode->data.start,0); 
		}

		free (inode); 
	}
}

/* Marks INODE to be deleted when it is closed by the last caller who
 * has it open. */
void
inode_remove (struct inode *inode) {
	ASSERT (inode != NULL);
	inode->removed = true;
}

//find page_cache page in hash. if not found, make one.
static struct page *
inode_hash_find(cluster_t clst){
	struct page *page;
	struct page temp;
	struct hash_elem *e;
	page_cache_initializer(&temp, VM_PAGE_CACHE, NULL);
	temp.page_cache.cluster_idx = (clst & (~0x7));

	e = hash_find(&open_inode_sectors, &temp.elem);
	if(e == NULL){	//if no page allocated for sector, allocate one
		page = (struct page *)malloc(sizeof(struct page));
		if(page==NULL) PANIC("new page_cache alloc failed");
		page_cache_initializer(page, VM_PAGE_CACHE, NULL);
		lock_init(&page->pglock);
		page->type = VM_PAGE_CACHE;
		page->page_cache.cluster_idx = (clst & (~0x7));
		e = hash_insert(&open_inode_sectors, &page->elem);
		if(e != NULL){
			free(page);
			page = hash_entry(e, struct page, elem);
		}
	}else{
		page = hash_entry(e, struct page, elem);
	}
	return page;
}
/* Reads SIZE bytes from INODE into BUFFER, starting at position OFFSET.
 * Returns the number of bytes actually read, which may be less
 * than SIZE if an error occurs or end of file is reached. */
off_t
inode_read_at (struct inode *inode, void *buffer_, off_t size, off_t offset) {
	uint8_t *buffer = buffer_;
	off_t bytes_read = 0;
	uint8_t *bounce = NULL;
	cluster_t cluster_idx = byte_to_cluster (inode, offset, false);
	cluster_t nxt_idx;
	char page_ofs;
	struct page *page;
	struct page *nxt_sector_page;
	if(cluster_idx==-1)
		return 0;
	int sector_ofs = offset % DISK_SECTOR_SIZE;		//TODO:need to change if sectors_per_cluster!=1
	off_t inode_left = inode_length (inode) - offset;
	while (size > 0) {
		/* Disk sector to read, starting byte offset within sector. */
		//disk_sector_t sector_idx = cluster_to_sector(cluster_idx);

		/* Bytes left in inode, bytes left in sector, lesser of the two. */
		int sector_left = DISK_SECTOR_SIZE - sector_ofs;
		int min_left = inode_left < sector_left ? inode_left : sector_left;

		/* Number of bytes to actually copy out of this sector. */
		int chunk_size = size < min_left ? size : min_left;
		if (chunk_size <= 0)
			break;
		page = inode_hash_find(cluster_idx);
		lock_acquire(&cache_lock);
		lock_acquire(&page->pglock);
		if(page->page_cache.cache_idx == -1){
			list_push_front(&swapin_queue,&page->list_elem);
			cond_signal(&not_empty, &cache_lock);
			while(page->page_cache.cache_idx == -1)
				cond_wait(&job_done, &cache_lock);

			nxt_idx = fat_get(cluster_idx);
			if(nxt_idx != EOChain){
				nxt_sector_page = inode_hash_find(nxt_idx);//read ahead
				list_push_front(&swapin_queue,&nxt_sector_page->list_elem);
			}
		
		}
		page_ofs = cluster_idx & 0x7;
		printf("%p %p %d\n",page->page_cache.kva, buffer, cluster_idx);
		memcpy(buffer + bytes_read, page->page_cache.kva+page_ofs*DISK_SECTOR_SIZE+sector_ofs,chunk_size); 
		page->page_cache.is_accessed = true;
		lock_release(&page->pglock);
		lock_release(&cache_lock);
		/* Advance. */
		size -= chunk_size;
		inode_left -= chunk_size;
		cluster_idx = fat_get(cluster_idx);
		bytes_read += chunk_size;
		sector_ofs = 0;
	}
	free (bounce);

	return bytes_read;
}
/* bounce buffer style. saving for just in case
		if (sector_ofs == 0 && chunk_size == DISK_SECTOR_SIZE) {
			disk_read (filesys_disk, sector_idx, buffer + bytes_read); 
		} else {
			if (bounce == NULL) {
				bounce = malloc (DISK_SECTOR_SIZE);
				if (bounce == NULL)
					break;
			}
			disk_read (filesys_disk, sector_idx, bounce);
			memcpy (buffer + bytes_read, bounce + sector_ofs, chunk_size);
		}
		*/


/* Writes SIZE bytes from BUFFER into INODE, starting at OFFSET.
 * Returns the number of bytes actually written, which may be
 * less than SIZE if end of file is reached or an error occurs.
 * (Normally a write at end of file would extend the inode, but
 * growth is not yet implemented.) */
off_t
inode_write_at (struct inode *inode, const void *buffer_, off_t size,
		off_t offset) {
	const uint8_t *buffer = buffer_;
	off_t bytes_written = 0;
	uint8_t *bounce = NULL;
	struct page * page;
	char page_ofs;
	cluster_t tmp;

	if (inode->deny_write_cnt)
		return 0;
	cluster_t cluster_idx = byte_to_cluster (inode, offset,true);
	int sector_ofs = offset % DISK_SECTOR_SIZE;
	//off_t inode_left = inode_length (inode) - offset;
	
	while (size > 0) {
		if(cluster_idx == EOChain){
			cluster_idx = fat_create_chain(tmp);
			inode->data.length += DISK_SECTOR_SIZE;
		}

		/* Sector to write, starting byte offset within sector. */
		//disk_sector_t sector_idx = cluster_to_sector(cluster_idx);

		/* Bytes left in inode, bytes left in sector, lesser of the two. */
		int sector_left = DISK_SECTOR_SIZE - sector_ofs;
		//int min_left = inode_left < sector_left ? inode_left : sector_left;

		/* Number of bytes to actually write into this sector. */
		//int chunk_size = size < min_left ? size : min_left;
		int chunk_size = size < sector_left ? size : sector_left;
		if (chunk_size <= 0)
			break;
		page = inode_hash_find(cluster_idx);
		lock_acquire(&page->pglock);
		if(page->page_cache.kva == NULL){
			lock_acquire(&cache_lock);
			list_push_front(&swapin_queue,&page->list_elem);
			cond_signal(&not_empty, &cache_lock);
			while(page->page_cache.kva == NULL)
				cond_wait(&job_done, &cache_lock);
			//no read ahead	
		
			lock_release(&cache_lock);
		}
		page_ofs = cluster_idx & 0x7;
		memcpy(page->page_cache.kva + page_ofs*DISK_SECTOR_SIZE + sector_ofs,buffer+bytes_written, chunk_size);
		bitmap_set(page->page_cache.swap_status, page_ofs, true);//set dirty bit
		page->page_cache.is_accessed = true;
		lock_release(&page->pglock);

		/* Advance. */
		size -= chunk_size;
	//	inode_left -= chunk_size;
		bytes_written += chunk_size;
		sector_ofs = 0;

		tmp = cluster_idx;
		cluster_idx = fat_get(cluster_idx);
	}
	free (bounce);
	
	return bytes_written;
}

/* bounce buffer impl. just in case
		if (sector_ofs == 0 && chunk_size == DISK_SECTOR_SIZE) {
			disk_write (filesys_disk, sector_idx, buffer + bytes_written); 
		} else {
			if (bounce == NULL) {
				bounce = malloc (DISK_SECTOR_SIZE);
				if (bounce == NULL)
					break;
			}

			if (sector_ofs > 0 || chunk_size < sector_left) 
				disk_read (filesys_disk, sector_idx, bounce);
			else
				memset (bounce, 0, DISK_SECTOR_SIZE);
			memcpy (bounce + sector_ofs, buffer + bytes_written, chunk_size);
			disk_write (filesys_disk, sector_idx, bounce); 
		}
*/


/* Disables writes to INODE.
   May be called at most once per inode opener. */
	void
inode_deny_write (struct inode *inode) 
{
	inode->deny_write_cnt++;
	ASSERT (inode->deny_write_cnt <= inode->open_cnt);
}

/* Re-enables writes to INODE.
 * Must be called once by each inode opener who has called
 * inode_deny_write() on the inode, before closing the inode. */
void
inode_allow_write (struct inode *inode) {
	ASSERT (inode->deny_write_cnt > 0);
	ASSERT (inode->deny_write_cnt <= inode->open_cnt);
	inode->deny_write_cnt--;
}

/* Returns the length, in bytes, of INODE's data. */
off_t
inode_length (const struct inode *inode) {
	return inode->data.length;
}
