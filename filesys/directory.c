#include "filesys/directory.h"
#include <stdio.h>
#include <string.h>
#include <list.h>
#include "filesys/filesys.h"
#include "filesys/inode.h"
#include "threads/malloc.h"
#include "threads/thread.h"
#include "userprog/process.h"
#include <hash.h>

const char *root_dir_str = "/";

static struct hash open_dirs;

static struct lock open_dirs_lock;

static unsigned dir_hash_func(const struct hash_elem*a, void *aux UNUSED);
static bool dir_hash_comp(const struct hash_elem *a,
							  const struct hash_elem *b, void *aux UNUSED);

void dir_init(void){
	lock_init(&open_dirs_lock);
	hash_init(&open_dirs, dir_hash_func, dir_hash_comp, NULL);
	thread_current()->process->cwd = dir_open_root();
	//printf("dir init pid %u cwd %p sector no %u\n", thread_current()->process->pid, thread_current()->process->cwd, thread_current()->process->cwd->sector);
}

/* Creates a directory with space for ENTRY_CNT entries in the
   given SECTOR.  Returns true if successful, false on failure. */
bool dir_create (block_sector_t sector, block_sector_t parent){
	//printf("dir create\n");
	struct dir *dir = NULL;
	bool success = inode_create (sector, 0, true)
			&& (dir = dir_open(inode_open(sector))) != NULL
			&& dir_add(dir, ".", sector) && dir_add(dir, "..", parent);

	//printf("number of files in dir is %u should be 2\n", dir_file_count(dir));
	dir_close(dir);

	return success;
}

/* Opens and returns the directory for the given INODE, of which
   it takes ownership.  Returns a null pointer on failure. */
struct dir *dir_open (struct inode *inode){
	//printf("dir open\n");
	if(inode == NULL){
		return NULL;
	}
	struct dir key_dir, *ret_dir;
	key_dir.sector = inode->sector;
	struct hash_elem *ret_elem;
	lock_acquire(&open_dirs_lock);
	ret_elem = hash_find(&open_dirs, &key_dir.e);
	if(ret_elem != NULL){
		ret_dir = hash_entry(ret_elem, struct dir, e);
		lock_acquire(&ret_dir->dir_lock);
		ret_dir->open_cnt ++;
		lock_release(&ret_dir->dir_lock);
		lock_release(&open_dirs_lock);
		//printf("Already existed\n");
		return ret_dir;

	}else{
		ret_dir = calloc(1, sizeof(struct dir));
		if(ret_dir == NULL){
			lock_release(&open_dirs_lock);
			inode_close(inode);
			free(ret_dir);
			return NULL;
		}
		ret_dir->inode = inode;
		ret_dir->sector = inode->sector;
		lock_init(&ret_dir->dir_lock);
		ret_dir->open_cnt = 1;
		ret_elem = hash_insert(&open_dirs, &ret_dir->e);
		lock_release(&open_dirs_lock);
		if(ret_elem != NULL){
			inode_close(inode);
			free(ret_dir);
			//printf("returned null\n");
			return NULL;
		}else{
			//printf("returned new sector %u \n", ret_dir->sector);
			return ret_dir;
		}
	}
}

/* Opens the root directory and returns a directory for it.
   Return true if successful, false on failure. */
struct dir *dir_open_root (void){
	//printf("dir open root\n");
	return dir_open (inode_open (ROOT_DIR_SECTOR));
}

/* Opens and returns a new directory for the same inode as DIR.
   Returns a null pointer on failure. */
struct dir *dir_reopen (struct dir *dir){
	if(dir == NULL){
		return NULL;
	}
	//printf("dir reopen\n");
	return dir_open (inode_reopen (dir->inode));
}

/* Destroys DIR and frees associated resources. */
void dir_close (struct dir *dir){
	//printf("dir close\n");
	if(dir == NULL){
		return;
	}

	struct hash_elem *ret_elem;
	lock_acquire(&open_dirs_lock);
	lock_acquire(&dir->dir_lock);
	bool delete = (--dir->open_cnt == 0);
	lock_release(&dir->dir_lock);

	if(delete){
		//printf("actually deleted\n");
		ret_elem =	hash_delete(&open_dirs, &dir->e);
		ASSERT(hash_entry(ret_elem, struct dir, e) == dir);
		inode_close(dir->inode);
		free(dir);
	}
	lock_release(&open_dirs_lock);
}

/* Returns the inode encapsulated by DIR. */
struct inode *dir_get_inode (struct dir *dir){
	//printf("dir get inode\n");
	if(dir == NULL){
		return NULL;
	}
	/* If it isn't null then we know that it has opencnt > 0
	   and the dir inode never changes after creation */
	return dir->inode;
}

/* Searches DIR for a file with the given NAME.
   If successful, returns true, sets *EP to the directory entry
   if EP is non-null, and sets *OFSP to the byte offset of the
   directory entry if OFSP is non-null.
   otherwise, returns false and ignores EP and OFSP. */
static bool lookup (const struct dir *dir, const char *name,
		struct dir_entry *ep, off_t *ofsp){
	//printf("lookup\n");

	if(dir == NULL|| name == NULL){
		return false;
	}

	ASSERT(lock_held_by_current_thread(&dir->dir_lock));
	struct dir_entry e;
	size_t ofs;

	ASSERT (dir != NULL);
	ASSERT (name != NULL);

	/* For loop checks all files in this directory and compares them to
	   the file name to verify that there are no files with the same name
	   in the directory*/
	for(ofs = 0; inode_read_at (dir->inode, &e, sizeof(e), ofs) == sizeof(e);
			ofs += sizeof(e) ){
		if(e.in_use && !strcmp (name, e.name)){
			if(ep != NULL){
				*ep = e;
			}
			if(ofsp != NULL){
				*ofsp = ofs;
			}
			//printf("lookup true\n");
			return true;
		}
	}
	//printf("lookup false\n");
	return false;
}

/* Splits full into a directory path and a leaf file. Returns each
   in the pointers path and leaf. And the return value is true if
   the path is relative and false if it is absolute. leaf will be
   returned NULL if there is no leaf in the full path passed in
   path will be null if there is only a leaf in the path name.
   Otherwise on return the path will point to the path of the
   file and leaf will point to the leaf of the file and the forward
   slash seperating them will be turned into a null character.
   Asserts that the full path passed in has a size greater than 0 */
static bool dir_path_and_leaf(char *full, char **path, char **leaf){
	//printf("dir path and leaf\n");
	uint32_t last_slash = 0;
	uint32_t count = 0;
	if(full == NULL || path == NULL || leaf == NULL || strlen(full) == 0){
		return false;
	}

	while(full[count] != '\0'){
		if(full[count] == '/'){
			last_slash = count;
		}
		count ++;
	}
	bool is_relative = false;
	if(*full == '/'){
		if(last_slash == 0){
			if(count == 1){
				/* The root is the only thing passed in*/
				*leaf = full;
			}else{
				/*the leaf is in the root dir*/
				*leaf = (full + 1);
			}
			*path = NULL;
		}else{
			/*Leaf is at end of path */
			*path = full;
			full[last_slash] = '\0';
			*leaf = (full + last_slash + 1);
		}
	}else{
		is_relative = true;
		if(last_slash == 0){
			/* leaf is only thing handed in */
			*path = NULL;
			*leaf = full;
		}else{
			/*The leaf is at the end of the path */
			*path = full;
			full[last_slash] = '\0';
			*leaf = (full + last_slash + 1);
		}
	}
	if(**leaf == '\0'){
		/* No leaf at end of string :( */
		*leaf = NULL;
	}
	return is_relative;

}

/* Recursive function, traverses the path till it finds the last item in the
   path. Opens the last item as a directory and returns it. Returns NULL if
   the last item in the path is not a directory*/
static struct dir *dir_open_path_wrap(const char *path,
			    struct dir *start_dir, bool first_call){
	//printf("open path recursive\n");
	if(path == NULL || start_dir == NULL){
		//printf("opr ret null 1\n");
		return NULL;
	}
	//printf("dir open path wrap\n");
	bool return_root = false;
	if(*path == '\0'){
		return NULL;
	}
	if(*path == '/' && first_call){
		return_root = true;
	}
	while(*path == '/'){
		path ++;
	}

	//printf("past /\n");

	if(*path == '\0'){
		/* path ended in a \ */
		if(return_root){
			//printf("opr return root 1\n");
			return dir_open_root();
		}else{
			//printf("opr return NULL 1\n");
			return NULL;
		}
	}
	char buf[NAME_MAX+1];
	memset(buf, 0, NAME_MAX+1);
	uint32_t name_chars = 0;
	while(*path != '\0' && *path != '/' && name_chars != NAME_MAX + 1){
		buf[name_chars] = *path;
		name_chars ++;
		path ++;
	}

	if(*path != '\0' && *path != '/'){
		/* A file name was too long */
		//printf("opr name too long 1\n");
		return NULL;
	}

	struct dir_entry e;
	lock_acquire(&start_dir->dir_lock);
	if(lookup (start_dir, buf, &e, NULL)){
		lock_release(&start_dir->dir_lock);
		struct inode *ino = inode_open(e.inode_sector);
		if(!inode_is_dir(ino)){
			inode_close(ino);
			//printf("inode wasn't a directory returned nULL\n");
			return NULL;
		}

		struct dir *next_dir = dir_open(ino);
		if(*path == '\0'){
			//printf("Returned next dir\n");
			return next_dir;
		}else{
			struct dir *ret = dir_open_path_wrap(path, next_dir, false);
			dir_close(next_dir);
			//printf("opr returned from recursion!\n");
			return ret;
		}
	}else{
		lock_release(&start_dir->dir_lock);
		//printf("opr return NULL\n");
		return NULL;
	}
}


/* opens the directory and returns the leaf of the path if any. Paths
   with a trailing / are illegal file names. A path to a directory with
   a trailing / will open the directory and set file_name to point to
   NULL.*/
struct dir *dir_open_path(const char *path, const char **file_name){
	//printf("dir open path %s \n", path);
	uint32_t path_length;
	if(path == NULL || (path_length = strlen(path)) == 0 || file_name == NULL){
		//printf("Failed first\n");
		return NULL;
	}
	char buf [path_length + 1];
	memcpy(buf, path, path_length + 1); /* Copy all and null term */
	char *dir_path = NULL;
	char *file_leaf = NULL;
	bool is_relative = dir_path_and_leaf(buf, &dir_path, &file_leaf);
	if(file_leaf == NULL){
		*file_name = NULL;
	}else{
		*file_name = path + (file_leaf - buf);
	}
	if(is_relative){
		struct dir *cwd = thread_current()->process->cwd;
		if(dir_path == NULL){
			//printf("returned cwd %p %u pid %u\n", cwd, cwd->sector, thread_current()->process->pid);
			return dir_reopen(cwd);
		}else{
			//printf("parsed path was %s\n", dir_path);
			return dir_open_path_wrap(dir_path, cwd, false);
		}
	}else{
		struct dir* root = dir_open_root();
		if(dir_path == NULL){
			//printf("returned root 1\n");
			return root;
		}else{
			//printf("parsed path was %s\n", dir_path);
			struct dir *ret = dir_open_path_wrap(dir_path, root, true);
			if(ret == NULL){
				dir_close(root);
				//printf("returned null 2\n");
				return NULL;
			}
			if(ret->inode->sector == root->inode->sector){
				dir_close(ret);
				//printf("returned root 2\n");
				*file_name = root_dir_str;
				return root;
			}else{
				dir_close(root);
				//printf("returned non root directory\n");
				return ret;
			}
		}
	}
}


/* Searches DIR for a file with the given NAME
   and returns true if one exists, false otherwise.
   On success, sets *INODE to an inode for the file, otherwise to
   a null pointer.  The caller must close *INODE. */
bool dir_lookup (struct dir *dir, const char *name, struct inode **inode){
	if(dir == NULL|| name == NULL || inode== NULL){
		return false;
	}
	//printf("dir lookup\n");
	struct dir_entry e;
	lock_acquire(&dir->dir_lock);
	if(lookup (dir, name, &e, NULL)){
		*inode = inode_open (e.inode_sector);
	}else{
		*inode = NULL;
	}
	lock_release(&dir->dir_lock);
	return *inode != NULL;
}

/* Adds a file named NAME to DIR, which must not already contain a
   file by that name.  The file's inode is in sector
   INODE_SECTOR. Returns true if successful, false on failure.
   Fails if NAME is invalid (i.e. too long) or a disk or memory
   error occurs. */
bool dir_add (struct dir *dir, const char *name, block_sector_t inode_sector){
	if(dir == NULL || name == NULL || strlen(name) == 0){
		return false;
	}
	//printf("dir add %p\n", dir);
	struct dir_entry e;
	off_t ofs;
	bool success = false;

	ASSERT (dir != NULL);
	ASSERT (name != NULL);

	/* Check NAME for validity. */
	if(*name == '\0' || strlen (name) > NAME_MAX){
		return false;
	}

	lock_acquire(&dir->dir_lock);

	/* Check that NAME is not in use. */
	if(lookup (dir, name, NULL, NULL)){
		goto done;
	}

	/* Set OFS to offset of free slot.
     If there are no free slots, then it will be set to the
     current end-of-file.

     inode_read_at() will only return a short read at end of file.
     Otherwise, we'd need to verify that we didn't get a short
     read due to something intermittent such as low memory. */
	for(ofs = 0; inode_read_at (dir->inode, &e, sizeof(struct dir_entry), ofs)
			== sizeof(struct dir_entry);
			ofs += sizeof(struct dir_entry)){
		if(!e.in_use){
			break;
		}
	}
	/* Write slot. */
	e.in_use = true;
	strlcpy (e.name, name, sizeof(e.name));
	e.inode_sector = inode_sector;
	success = inode_write_at (dir->inode, &e, sizeof(struct dir_entry), ofs)
			== sizeof(struct dir_entry);

	done:
	lock_release(&dir->dir_lock);

	//printf("added\n");
	return success;
}

/* Removes any entry for NAME in DIR.
   Returns true if successful, false on failure,
   which occurs only if there is no file with the given NAME. */
bool dir_remove (struct dir *dir, const char *name){
	if(dir == NULL || name == NULL || strlen(name) == 0){
		return false;
	}

	//printf("dir remove\n");
	struct dir_entry e;
	struct inode *inode = NULL;
	bool success = false;
	off_t ofs;

	ASSERT (dir != NULL);
	ASSERT (name != NULL);

	/* Used to query if the file we are evicting
	   is actually a directory and querying if it
	   is in use */
	lock_acquire(&open_dirs_lock);
	lock_acquire(&dir->dir_lock);
	/* Find directory entry. */
	if(!lookup (dir, name, &e, &ofs)){
		goto done;
	}

	/* Open inode. */
	inode = inode_open (e.inode_sector);
	if(inode == NULL){
		goto done;
	}

	if(inode_is_dir(inode)){
		/* remove the director if no one else is
		   using it right at this moment and the
		   directory is empty*/
		struct dir key_dir;
		key_dir.sector = inode->sector;
		struct hash_elem *ret_elem;
		ret_elem = hash_find(&open_dirs, &key_dir.e);
		if(ret_elem != NULL){
			lock_release(&dir->dir_lock);
			lock_release(&open_dirs_lock);
			goto done;
		}
		lock_release(&dir->dir_lock);
		lock_release(&open_dirs_lock);

		struct dir *sub_dir = dir_open(inode);


		uint32_t file_count = dir_file_count(sub_dir);
		dir_close(sub_dir);
		if(file_count != 0){
			goto done;
		}
	}else{
		lock_release(&open_dirs_lock);
	}

	/* Erase directory entry. */
	e.in_use = false;
	if(inode_write_at (dir->inode, &e, sizeof(e), ofs) != sizeof(e)){
		goto done;
	}

	/* Remove inode. */
	inode_remove (inode);
	success = true;
	lock_release(&dir->dir_lock);

	done:
	inode_close (inode);
	return success;
}

/* Reads the next directory entry in DIR and stores the name in
   NAME. Starts from OFF Returns true if successful, false
   if the directory contains no more entries. Changes OFF.
   Call the first time with off of 0. Inspired by strtok_r.
   Will return with name null terminated... To be real will completely
   clear out the buffer before putting anythin it it*/
bool dir_readdir (struct dir *dir, char name[NAME_MAX + 1], off_t *off){
	if(dir == NULL || name == NULL || off == NULL){
		return false;
	}
	memset(name, 0, NAME_MAX + 1);
	//printf("dir readdir\n");
	struct dir_entry e;
	lock_acquire(&dir->dir_lock);
	while(inode_read_at (dir->inode, &e, sizeof(struct dir_entry), *off)
			== sizeof(struct dir_entry)){
		*off += sizeof(struct dir_entry);
		if(e.in_use){
			strlcpy (name, e.name, NAME_MAX + 1);
			lock_release(&dir->dir_lock);
			return true;
		}
	}
	lock_release(&dir->dir_lock);
	return false;
}

uint32_t dir_file_count(struct dir *dir){
	if(dir == NULL){
		return 0;
	}
	//printf("dir file_count\n");
	lock_acquire(&dir->dir_lock);
	off_t off = 0;
	uint32_t file_count = 0;
	struct dir_entry e;
	while(inode_read_at(dir->inode, &e, sizeof(struct dir_entry), off)
			== sizeof(struct dir_entry)){

		off += sizeof(struct dir_entry);
		if(e.in_use){
			file_count ++;
		}
	}
	lock_release(&dir->dir_lock);
	return file_count;
}

static unsigned dir_hash_func(const struct hash_elem*a, void *aux UNUSED){
	block_sector_t key = hash_entry(a, struct dir, e)->sector;
	return hash_bytes(&key, (sizeof(uint32_t)));
}

static bool dir_hash_comp(const struct hash_elem *a,
							  const struct hash_elem *b, void *aux UNUSED){
	ASSERT(a != NULL);
	ASSERT(b != NULL);
	return (hash_entry(a, struct dir, e)->sector <
			hash_entry(b, struct dir, e)->sector);
}

