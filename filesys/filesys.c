#include "filesys/filesys.h"
#include <debug.h>
#include <stdio.h>
#include <string.h>
#include "filesys/file.h"
#include "filesys/free-map.h"
#include "filesys/fat.h"
#include "filesys/inode.h"
#include "filesys/directory.h"
#include "threads/vaddr.h"
#include "vm/vm.h"
#include "devices/disk.h"

/* The disk that contains the file system. */
struct disk *filesys_disk;

static void do_format (void);

/* Initializes the file system module.
 * If FORMAT is true, reformats the file system. */
void
filesys_init (bool format) {
	filesys_disk = disk_get (0, 1);
	if (filesys_disk == NULL)
		PANIC ("hd0:1 (hdb) not present, file system initialization failed");

	inode_init ();

#ifdef EFILESYS
	page_cache_init();
	fat_init ();

	if (format)
		do_format ();

	fat_open ();
#else
	/* Original FS */
	free_map_init ();

	if (format)
		do_format ();

	free_map_open ();
#endif
}

/* Shuts down the file system module, writing any unwritten data
 * to disk. */
void
filesys_done (void) {
	/* Original FS */
#ifdef EFILESYS
	inode_done();
	page_cache_close();
	fat_close ();
#else
	free_map_close ();
#endif
}

/* Creates a file named NAME with the given INITIAL_SIZE.
 * Returns true if successful, false otherwise.
 * Fails if a file named NAME already exists,
 * or if internal memory allocation fails. */
bool
filesys_create (const char *name, off_t initial_size) {
	cluster_t inode_cluster = 0;
	struct dir *dir;
	struct inode *inode = NULL;
	char file_name[NAME_MAX+1];

	if(dir_search_dir(thread_current()->cur_dir, name, &inode, file_name)){
		dir = dir_open(inode);
	}else return false;

	bool success = (dir != NULL
			&& free_fat_allocate (1, &inode_cluster)
			&& inode_create (inode_cluster, initial_size, INODE_FILE)
			&& dir_add (dir, file_name, inode_cluster));
	if (!success && inode_cluster != 0)
		fat_remove_chain (inode_cluster,0);
	dir_close (dir);

	return success;
}

/* Opens the file with the given NAME.
 * Returns the new file if successful or a null pointer
 * otherwise.
 * Fails if no file named NAME exists,
 * or if an internal memory allocation fails. */
struct file *
filesys_open (const char *name) {
	struct dir *dir;
	struct inode *inode = NULL;
	char file_name[NAME_MAX+1];
	if(dir_search_dir(thread_current()->cur_dir, name, &inode, file_name)){
		dir = dir_open(inode);
	}else return NULL;
	
	if (dir != NULL)
		dir_lookup (dir, file_name, &inode);
	
	while( inode != NULL && inode_type(inode) == INODE_SYMLINK){
		char *target = palloc_get_page(0);
		inode_read_at(inode,target,PGSIZE,0);
		if(dir_search_dir(dir, target, &inode, file_name)){
			dir_close(dir);
			dir = dir_open(inode);
			inode = NULL;
			if(dir != NULL)
				dir_lookup(dir, file_name, &inode);
			palloc_free_page(target);
		}else{
			palloc_free_page(target);
			return NULL;
		}
	}
	dir_close (dir);
	return file_open (inode);
}

/* Deletes the file named NAME.
 * Returns true if successful, false on failure.
 * Fails if no file named NAME exists,
 * or if an internal memory allocation fails. */
bool
filesys_remove (const char *name) {
	struct dir *dir;
	struct inode *inode = NULL;
	char file_name[NAME_MAX+1];
	if(dir_search_dir(thread_current()->cur_dir, name, &inode, file_name)){
		dir = dir_open(inode);
	}else return false;

	bool success = dir != NULL && dir_remove (dir, file_name);
	dir_close (dir);

	return success;
}

bool 
filesys_chdir(const char *name){
	struct dir *dir;
	struct inode *inode = NULL;
	char file_name[NAME_MAX+1];
	
	if(dir_search_dir(thread_current()->cur_dir, name, &inode, file_name)){
		dir = dir_open(inode);
	}else return false;
	
	inode= NULL;
	if(dir != NULL)
		dir_lookup(dir, file_name, &inode);

	if(inode != NULL){
		thread_current()->cur_dir = dir_open(inode);
		return true;
	}
	return false;
}

bool
filesys_mkdir(const char *name){
	cluster_t inode_cluster=0;
	struct dir *dir;
	struct inode *inode = NULL;
	char file_name[NAME_MAX+1];
	
	if(dir_search_dir(thread_current()->cur_dir, name, &inode, file_name)){
		dir = dir_open(inode);
	}else return false;
	
	bool success = (dir != NULL
			&& free_fat_allocate (1, &inode_cluster)
			&& dir_create (inode_cluster, 1, inode_get_inumber(dir_get_inode(dir)))
			&& dir_add (dir, file_name, inode_cluster));
	if (!success && inode_cluster != 0)
		fat_remove_chain (inode_cluster,0);
	dir_close (dir);
	return success;
}

int
filesys_symlink(const char *target, const char *linkpath){
	cluster_t inode_cluster = 0;
	struct dir *dir;
	struct inode *inode = NULL;
	char file_name[NAME_MAX+1];

	if(dir_search_dir(thread_current()->cur_dir, linkpath, &inode, file_name)){
		dir = dir_open(inode);
	}else return -1;

	bool success = (dir != NULL
			&& free_fat_allocate (1, &inode_cluster)
			&& inode_create (inode_cluster, strlen(target)+1, INODE_SYMLINK)
			&& dir_add (dir, file_name, inode_cluster));
	if (!success && inode_cluster != 0)
		fat_remove_chain (inode_cluster,0);
	dir_close (dir);

	inode = inode_open(inode_cluster);
	inode_write_at(inode, target, strlen(target)+1,0);
	inode_close(inode);
	
	if(success)
		return 0;
	else return -1;
}

/* Formats the file system. */
static void
do_format (void) {
	printf ("Formatting file system...");

#ifdef EFILESYS
	/* Create FAT and save it to the disk. */
	fat_create ();
	if (!dir_create (ROOT_DIR_SECTOR, 16, 0))
		PANIC ("root directory creation failed");
	fat_close ();
#else
	free_map_create ();
	if (!dir_create (ROOT_DIR_SECTOR, 16))
		PANIC ("root directory creation failed");
	free_map_close ();
#endif

	printf ("done.\n");
}
