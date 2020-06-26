#include "filesys/inode.h"
#include <list.h>
#include <debug.h>
#include <round.h>
#include <string.h>
#include "filesys/filesys.h"
#include "filesys/free-map.h"
#include "filesys/fat.h"
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

/* Initializes the inode module. */
void
inode_init (void) {
	list_init (&open_inodes);
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

/* Reads SIZE bytes from INODE into BUFFER, starting at position OFFSET.
 * Returns the number of bytes actually read, which may be less
 * than SIZE if an error occurs or end of file is reached. */
off_t
inode_read_at (struct inode *inode, void *buffer_, off_t size, off_t offset) {
	uint8_t *buffer = buffer_;
	off_t bytes_read = 0;
	uint8_t *bounce = NULL;
	cluster_t cluster_idx = byte_to_cluster (inode, offset, false);
	if(cluster_idx==-1)
		return 0;
	int sector_ofs = offset % DISK_SECTOR_SIZE;		//TODO:need to change if sectors_per_cluster!=1
	off_t inode_left = inode_length (inode) - offset;
	while (size > 0) {
		/* Disk sector to read, starting byte offset within sector. */
		disk_sector_t sector_idx = cluster_to_sector(cluster_idx);

		/* Bytes left in inode, bytes left in sector, lesser of the two. */
		int sector_left = DISK_SECTOR_SIZE - sector_ofs;
		int min_left = inode_left < sector_left ? inode_left : sector_left;

		/* Number of bytes to actually copy out of this sector. */
		int chunk_size = size < min_left ? size : min_left;
		if (chunk_size <= 0)
			break;

		if (sector_ofs == 0 && chunk_size == DISK_SECTOR_SIZE) {
			/* Read full sector directly into caller's buffer. */
			disk_read (filesys_disk, sector_idx, buffer + bytes_read); 
		} else {
			/* Read sector into bounce buffer, then partially copy
			 * into caller's buffer. */
			if (bounce == NULL) {
				bounce = malloc (DISK_SECTOR_SIZE);
				if (bounce == NULL)
					break;
			}
			disk_read (filesys_disk, sector_idx, bounce);
			memcpy (buffer + bytes_read, bounce + sector_ofs, chunk_size);
		}

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
		disk_sector_t sector_idx = cluster_to_sector(cluster_idx);

		/* Bytes left in inode, bytes left in sector, lesser of the two. */
		int sector_left = DISK_SECTOR_SIZE - sector_ofs;
		//int min_left = inode_left < sector_left ? inode_left : sector_left;

		/* Number of bytes to actually write into this sector. */
		//int chunk_size = size < min_left ? size : min_left;
		int chunk_size = size < sector_left ? size : sector_left;
		if (chunk_size <= 0)
			break;

		if (sector_ofs == 0 && chunk_size == DISK_SECTOR_SIZE) {
			/* Write full sector directly to disk. */
			disk_write (filesys_disk, sector_idx, buffer + bytes_written); 
		} else {
			/* We need a bounce buffer. */
			if (bounce == NULL) {
				bounce = malloc (DISK_SECTOR_SIZE);
				if (bounce == NULL)
					break;
			}

			/* If the sector contains data before or after the chunk
			   we're writing, then we need to read in the sector
			   first.  Otherwise we start with a sector of all zeros. */
			if (sector_ofs > 0 || chunk_size < sector_left) 
				disk_read (filesys_disk, sector_idx, bounce);
			else
				memset (bounce, 0, DISK_SECTOR_SIZE);
			memcpy (bounce + sector_ofs, buffer + bytes_written, chunk_size);
			disk_write (filesys_disk, sector_idx, bounce); 
		}

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
