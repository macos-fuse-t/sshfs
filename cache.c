/*
  Caching file system proxy
  Copyright (C) 2004  Miklos Szeredi <miklos@szeredi.hu>

  This program can be distributed under the terms of the GNU GPL.
  See the file COPYING.
*/

#include "cache.h"
#include <stdio.h>
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <glib.h>
#include <pthread.h>
#include <sys/stat.h>

#define DEFAULT_CACHE_TIMEOUT_SECS 20
#define DEFAULT_MAX_CACHE_SIZE 10000
#define DEFAULT_CACHE_CLEAN_INTERVAL_SECS 60
#define DEFAULT_MIN_CACHE_CLEAN_INTERVAL_SECS 5

struct cache {
	int on;
	unsigned int stat_timeout_secs;
	unsigned int dir_timeout_secs;
	unsigned int link_timeout_secs;
	unsigned int max_size;
	unsigned int clean_interval_secs;
	unsigned int min_clean_interval_secs;
	struct fuse_operations *next_oper;
	GHashTable *table;
	pthread_mutex_t lock;
	time_t last_cleaned;
	uint64_t write_ctr;
	uint64_t namespace_ctr;
};

static struct cache cache;

struct node {
	struct stat stat;
	int stat_negative;
	time_t stat_valid;
	GPtrArray *dir;
	time_t dir_valid;
	char *link;
	time_t link_valid;
	time_t valid;
};

struct readdir_handle {
	const char *path;
	GPtrArray *dir;
	uint64_t wrctr;
	uint64_t namespace_ctr;
};

struct file_handle {
	/* Did we send an open request to the underlying fs? */
	int is_open;

	/* If so, this will hold its handle */
	unsigned long fs_fh;
};

struct cache_dirent {
	char *name;
	struct stat stat;
	enum fuse_fill_dir_flags flags;
};

static void free_node(gpointer node_)
{
	struct node *node = (struct node *) node_;
	if (node->dir != NULL) {
		g_ptr_array_free(node->dir, TRUE);
  }
	g_free(node);
}

static void free_cache_dirent(gpointer data) {
	struct cache_dirent *cache_dirent = (struct cache_dirent *) data;
	if (cache_dirent != NULL) {
		g_free(cache_dirent->name);
		g_free(cache_dirent);
	}
}

static int cache_clean_entry(void *key_, struct node *node, time_t *now)
{
	(void) key_;
	if (*now > node->valid)
		return TRUE;
	else
		return FALSE;
}

static void cache_clean(void)
{
	time_t now = time(NULL);
	if (now > cache.last_cleaned + cache.min_clean_interval_secs &&
	    (g_hash_table_size(cache.table) > cache.max_size ||
	     now > cache.last_cleaned + cache.clean_interval_secs)) {
		g_hash_table_foreach_remove(cache.table,
					    (GHRFunc) cache_clean_entry, &now);
		cache.last_cleaned = now;
	}
}

static struct node *cache_lookup(const char *path)
{
	return (struct node *) g_hash_table_lookup(cache.table, path);
}

static void cache_purge(const char *path)
{
	g_hash_table_remove(cache.table, path);
}

static void cache_purge_parent(const char *path)
{
	const char *s = strrchr(path, '/');
	if (s) {
		if (s == path)
			g_hash_table_remove(cache.table, "/");
		else {
			char *parent = g_strndup(path, s - path);
			cache_purge(parent);
			g_free(parent);
		}
	}
}

void cache_invalidate(const char *path)
{
	pthread_mutex_lock(&cache.lock);
	cache_purge(path);
	pthread_mutex_unlock(&cache.lock);
}

static void cache_invalidate_write(const char *path)
{
	pthread_mutex_lock(&cache.lock);
	cache_purge(path);
	cache.write_ctr++;
	pthread_mutex_unlock(&cache.lock);
}

static void cache_invalidate_dir(const char *path)
{
	pthread_mutex_lock(&cache.lock);
	cache_purge(path);
	cache_purge_parent(path);
	cache.namespace_ctr++;
	pthread_mutex_unlock(&cache.lock);
}

static int cache_del_children(const char *key, void *val_, const char *path)
{
	(void) val_;
	if (strncmp(key, path, strlen(path)) == 0)
		return TRUE;
	else
		return FALSE;
}

static void cache_do_rename(const char *from, const char *to)
{
	pthread_mutex_lock(&cache.lock);
	g_hash_table_foreach_remove(cache.table, (GHRFunc) cache_del_children,
				    (char *) from);
	cache_purge(from);
	cache_purge(to);
	cache_purge_parent(from);
	cache_purge_parent(to);
	cache.namespace_ctr++;
	pthread_mutex_unlock(&cache.lock);
}

static struct node *cache_get(const char *path)
{
	struct node *node = cache_lookup(path);
	if (node == NULL) {
		char *pathcopy = g_strdup(path);
		node = g_new0(struct node, 1);
		g_hash_table_insert(cache.table, pathcopy, node);
	}
	return node;
}

void cache_add_attr(const char *path, const struct stat *stbuf, uint64_t wrctr)
{
	struct node *node;

	pthread_mutex_lock(&cache.lock);
	if (wrctr == cache.write_ctr) {
		node = cache_get(path);
		node->stat = *stbuf;
		node->stat_negative = 0;
		node->stat_valid = time(NULL) + cache.stat_timeout_secs;
		if (node->stat_valid > node->valid)
			node->valid = node->stat_valid;
		cache_clean();
	}
	pthread_mutex_unlock(&cache.lock);
}

static void cache_add_negative_attr(const char *path, uint64_t wrctr)
{
	struct node *node;

	pthread_mutex_lock(&cache.lock);
	if (wrctr == cache.write_ctr) {
		node = cache_get(path);
		memset(&node->stat, 0, sizeof(node->stat));
		node->stat_negative = 1;
		node->stat_valid = time(NULL) + cache.stat_timeout_secs;
		if (node->stat_valid > node->valid)
			node->valid = node->stat_valid;
		cache_clean();
	}
	pthread_mutex_unlock(&cache.lock);
}

static void cache_add_dir(const char *path, GPtrArray *dir)
{
	struct node *node;

	pthread_mutex_lock(&cache.lock);
	node = cache_get(path);
	if (node->dir != NULL) {
		g_ptr_array_free(node->dir, TRUE);
  }
	node->dir = dir;
	node->dir_valid = time(NULL) + cache.dir_timeout_secs;
	if (node->dir_valid > node->valid)
		node->valid = node->dir_valid;
	cache_clean();
	pthread_mutex_unlock(&cache.lock);
}

static size_t my_strnlen(const char *s, size_t maxsize)
{
	const char *p;
	for (p = s; maxsize && *p; maxsize--, p++);
	return p - s;
}

static void cache_add_link(const char *path, const char *link, size_t size)
{
	struct node *node;

	pthread_mutex_lock(&cache.lock);
	node = cache_get(path);
	g_free(node->link);
	node->link = g_strndup(link, my_strnlen(link, size-1));
	node->link_valid = time(NULL) + cache.link_timeout_secs;
	if (node->link_valid > node->valid)
		node->valid = node->link_valid;
	cache_clean();
	pthread_mutex_unlock(&cache.lock);
}

static int cache_get_attr(const char *path, struct stat *stbuf)
{
	struct node *node;
	int err = -EAGAIN;
	pthread_mutex_lock(&cache.lock);
	node = cache_lookup(path);
	if (node != NULL) {
		time_t now = time(NULL);
		if (node->stat_valid - now >= 0) {
			if (node->stat_negative) {
				err = -ENOENT;
			} else {
				*stbuf = node->stat;
				err = 0;
			}
		}
	}
	pthread_mutex_unlock(&cache.lock);
	return err;
}

static char *cache_fullpath(const char *dirpath, const char *name)
{
	const char *basepath = !dirpath[1] ? "" : dirpath;
	return g_strdup_printf("%s/%s", basepath, name);
}

#ifdef __APPLE__
static void cache_seed_appledouble_negatives(const char *path, GPtrArray *dir,
					     uint64_t wrctr)
{
	GHashTable *names;
	struct cache_dirent **cdent;

	names = g_hash_table_new(g_str_hash, g_str_equal);
	if (!names)
		return;

	for (cdent = (struct cache_dirent **) dir->pdata; *cdent != NULL; cdent++) {
		g_hash_table_add(names, (*cdent)->name);
	}

	for (cdent = (struct cache_dirent **) dir->pdata; *cdent != NULL; cdent++) {
		char *sidecar_name;
		char *sidecar_path;
		const char *name = (*cdent)->name;

		if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0)
			continue;
		if (strncmp(name, "._", 2) == 0)
			continue;

		sidecar_name = g_strdup_printf("._%s", name);
		if (g_hash_table_contains(names, sidecar_name)) {
			g_free(sidecar_name);
			continue;
		}

		sidecar_path = cache_fullpath(path, sidecar_name);
		cache_add_negative_attr(sidecar_path, wrctr);
		g_free(sidecar_path);
		g_free(sidecar_name);
	}

	g_hash_table_destroy(names);
}
#endif

uint64_t cache_get_write_ctr(void)
{
	uint64_t res;

	pthread_mutex_lock(&cache.lock);
	res = cache.write_ctr;
	pthread_mutex_unlock(&cache.lock);

	return res;
}

static uint64_t cache_get_namespace_ctr(void)
{
	uint64_t res;

	pthread_mutex_lock(&cache.lock);
	res = cache.namespace_ctr;
	pthread_mutex_unlock(&cache.lock);

	return res;
}

static int cache_readdir_snapshot_changed(uint64_t namespace_ctr)
{
	return cache_get_namespace_ctr() != namespace_ctr;
}

static void cache_readdir_fill_from_snapshot(void *buf, fuse_fill_dir_t filler,
					     GPtrArray *dir)
{
	struct cache_dirent **cdent;

	for (cdent = (struct cache_dirent **) dir->pdata; *cdent != NULL; cdent++) {
		if (filler(buf, (*cdent)->name, &(*cdent)->stat, 0,
			   (*cdent)->flags))
			break;
	}
}

static void cache_readdir_cache_snapshot(const char *path, GPtrArray *dir,
					 uint64_t wrctr)
{
	struct cache_dirent **cdent;

	for (cdent = (struct cache_dirent **) dir->pdata; *cdent != NULL; cdent++) {
		struct cache_dirent *entry = *cdent;
		if (entry->stat.st_mode & S_IFMT) {
			char *fullpath = cache_fullpath(path, entry->name);
			cache_add_attr(fullpath, &entry->stat, wrctr);
			g_free(fullpath);
		}
	}

#ifdef __APPLE__
	cache_seed_appledouble_negatives(path, dir, wrctr);
#endif
	cache_add_dir(path, dir);
}

static void *cache_init(struct fuse_conn_info *conn,
                        struct fuse_config *cfg)
{
	void *res;
	res = cache.next_oper->init(conn, cfg);

	// Cache requires a path for each request
	cfg->nullpath_ok = 0;

	return res;
}

static int cache_getattr(const char *path, struct stat *stbuf,
			 struct fuse_file_info *fi)
{
	int err = cache_get_attr(path, stbuf);
	if (err == -EAGAIN) {
		uint64_t wrctr = cache_get_write_ctr();
		err = cache.next_oper->getattr(path, stbuf, fi);
		if (!err)
			cache_add_attr(path, stbuf, wrctr);
	}
	return err;
}

static int cache_readlink(const char *path, char *buf, size_t size)
{
	struct node *node;
	int err;

	pthread_mutex_lock(&cache.lock);
	node = cache_lookup(path);
	if (node != NULL) {
		time_t now = time(NULL);
		if (node->link_valid - now >= 0) {
			strncpy(buf, node->link, size-1);
			buf[size-1] = '\0';
			pthread_mutex_unlock(&cache.lock);
			return 0;
		}
	}
	pthread_mutex_unlock(&cache.lock);
	err = cache.next_oper->readlink(path, buf, size);
	if (!err)
		cache_add_link(path, buf, size);

	return err;
}


static int cache_opendir(const char *path, struct fuse_file_info *fi)
{
	(void) path;
	struct file_handle *cfi;

	cfi = malloc(sizeof(struct file_handle));
	if(cfi == NULL)
		return -ENOMEM;
	cfi->is_open = 0;
	fi->fh = (unsigned long) cfi;
	return 0;
}

static int cache_releasedir(const char *path, struct fuse_file_info *fi)
{
	int err;
	struct file_handle *cfi;

	cfi = (struct file_handle*) fi->fh;

	if(cfi->is_open) {
		fi->fh = cfi->fs_fh;
		err = cache.next_oper->releasedir(path, fi);
	} else
		err = 0;

	free(cfi);
	return err;
}

static int cache_dirfill (void *buf, const char *name,
			  const struct stat *stbuf, off_t off,
			  enum fuse_fill_dir_flags flags)
{
	struct readdir_handle *ch;

	ch = (struct readdir_handle*) buf;
	(void) off;

	struct cache_dirent *cdent = g_malloc(sizeof(struct cache_dirent));
	cdent->name = g_strdup(name);
	cdent->stat = *stbuf;
	cdent->flags = flags;
	g_ptr_array_add(ch->dir, cdent);

	return 0;
}

static int cache_readdir(const char *path, void *buf, fuse_fill_dir_t filler,
			 off_t offset, struct fuse_file_info *fi,
			 enum fuse_readdir_flags flags)
{
	struct readdir_handle ch;
	struct file_handle *cfi;
	int err;
	GPtrArray *dir;
	struct node *node;
	int retries = 0;
	const int max_retries = 3;
	int stable_snapshot = 0;

	assert(offset == 0);

	pthread_mutex_lock(&cache.lock);
	node = cache_lookup(path);
	if (node != NULL && node->dir != NULL) {
		time_t now = time(NULL);
		if (node->dir_valid - now >= 0) {
			cache_readdir_fill_from_snapshot(buf, filler, node->dir);
			pthread_mutex_unlock(&cache.lock);
			return 0;
		}
	}
	pthread_mutex_unlock(&cache.lock);

	cfi = (struct file_handle*) fi->fh;
	if(cfi->is_open)
		fi->fh = cfi->fs_fh;
	else {
		if(cache.next_oper->opendir) {
			err = cache.next_oper->opendir(path, fi);
			if(err)
				return err;
		}
		cfi->is_open = 1;
		cfi->fs_fh = fi->fh;
	}

	for (;;) {
		ch.path = path;
		ch.dir = g_ptr_array_new();
		g_ptr_array_set_free_func(ch.dir, free_cache_dirent);
		ch.wrctr = cache_get_write_ctr();
		ch.namespace_ctr = cache_get_namespace_ctr();
		err = cache.next_oper->readdir(path, &ch, cache_dirfill, offset, fi,
					      flags);
		g_ptr_array_add(ch.dir, NULL);
		dir = ch.dir;
		if (err) {
			g_ptr_array_free(dir, TRUE);
			return err;
		}
		if (!cache_readdir_snapshot_changed(ch.namespace_ctr)) {
			stable_snapshot = 1;
			break;
		}
		if (retries >= max_retries)
			break;
		g_ptr_array_free(dir, TRUE);
		retries++;
	}

	if (!err) {
		if (stable_snapshot)
			cache_readdir_cache_snapshot(path, dir, ch.wrctr);
		cache_readdir_fill_from_snapshot(buf, filler, dir);
		if (!stable_snapshot)
			g_ptr_array_free(dir, TRUE);
	}

	return err;
}

static int cache_mknod(const char *path, mode_t mode, dev_t rdev)
{
	int err = cache.next_oper->mknod(path, mode, rdev);
	if (!err)
		cache_invalidate_dir(path);
	return err;
}

static int cache_mkdir(const char *path, mode_t mode)
{
	int err = cache.next_oper->mkdir(path, mode);
	if (!err)
		cache_invalidate_dir(path);
	return err;
}

static int cache_unlink(const char *path)
{
	int err = cache.next_oper->unlink(path);
	if (!err)
		cache_invalidate_dir(path);
	return err;
}

static int cache_rmdir(const char *path)
{
	int err = cache.next_oper->rmdir(path);
	if (!err)
		cache_invalidate_dir(path);
	return err;
}

static int cache_symlink(const char *from, const char *to)
{
	int err = cache.next_oper->symlink(from, to);
	if (!err)
		cache_invalidate_dir(to);
	return err;
}

static int cache_rename(const char *from, const char *to, unsigned int flags)
{
	int err = cache.next_oper->rename(from, to, flags);
	if (!err)
		cache_do_rename(from, to);
	return err;
}

static int cache_link(const char *from, const char *to)
{
	int err = cache.next_oper->link(from, to);
	if (!err) {
		cache_invalidate(from);
		cache_invalidate_dir(to);
	}
	return err;
}

static int cache_chmod(const char *path, mode_t mode,
                       struct fuse_file_info *fi)
{
	int err = cache.next_oper->chmod(path, mode, fi);
	if (!err)
		cache_invalidate(path);
	return err;
}

static int cache_chown(const char *path, uid_t uid, gid_t gid,
                       struct fuse_file_info *fi)
{
	int err = cache.next_oper->chown(path, uid, gid, fi);
	if (!err)
		cache_invalidate(path);
	return err;
}

static int cache_utimens(const char *path, const struct timespec tv[2],
			 struct fuse_file_info *fi)
{
	int err = cache.next_oper->utimens(path, tv, fi);
	if (!err)
		cache_invalidate(path);
	return err;
}

#if defined(__APPLE__) && defined(SETATTR_WANTS_MODE)
static int cache_setattr_x(const char *path, struct setattr_x *attr)
{
	int err = cache.next_oper->setattr_x(path, attr);
	if (!err)
		cache_invalidate(path);
	return err;
}

static int cache_fsetattr_x(const char *path, struct setattr_x *attr,
			    struct fuse_file_info *fi)
{
	int err = cache.next_oper->fsetattr_x(path, attr, fi);
	if (!err)
		cache_invalidate(path);
	return err;
}
#endif

static int cache_write(const char *path, const char *buf, size_t size,
                       off_t offset, struct fuse_file_info *fi)
{
	int res = cache.next_oper->write(path, buf, size, offset, fi);
	if (res >= 0)
		cache_invalidate_write(path);
	return res;
}

static int cache_create(const char *path, mode_t mode,
                        struct fuse_file_info *fi)
{
	int err = cache.next_oper->create(path, mode, fi);
	if (!err)
		cache_invalidate_dir(path);
	return err;
}

static int cache_truncate(const char *path, off_t size,
			  struct fuse_file_info *fi)
{
	int err = cache.next_oper->truncate(path, size, fi);
	if (!err)
		cache_invalidate(path);
	return err;
}

static void cache_fill(struct fuse_operations *oper,
		       struct fuse_operations *cache_oper)
{
	cache_oper->access   = oper->access;
	cache_oper->chmod    = oper->chmod ? cache_chmod : NULL;
	cache_oper->chown    = oper->chown ? cache_chown : NULL;
	cache_oper->create   = oper->create ? cache_create : NULL;
	cache_oper->flush    = oper->flush;
	cache_oper->fsync    = oper->fsync;
	cache_oper->getattr  = oper->getattr ? cache_getattr : NULL;
	cache_oper->getxattr = oper->getxattr;
	cache_oper->init     = cache_init;
	cache_oper->link     = oper->link ? cache_link : NULL;
	cache_oper->listxattr = oper->listxattr;
	cache_oper->mkdir    = oper->mkdir ? cache_mkdir : NULL;
	cache_oper->mknod    = oper->mknod ? cache_mknod : NULL;
	cache_oper->open     = oper->open;
	cache_oper->opendir  = cache_opendir;
	cache_oper->read     = oper->read;
	cache_oper->readdir  = oper->readdir ? cache_readdir : NULL;
	cache_oper->readlink = oper->readlink ? cache_readlink : NULL;
	cache_oper->release  = oper->release;
	cache_oper->releasedir = cache_releasedir;
	cache_oper->removexattr = oper->removexattr;
	cache_oper->rename   = oper->rename ? cache_rename : NULL;
	cache_oper->rmdir    = oper->rmdir ? cache_rmdir : NULL;
	cache_oper->setxattr = oper->setxattr;
	cache_oper->statfs   = oper->statfs;
	cache_oper->symlink  = oper->symlink ? cache_symlink : NULL;
	cache_oper->truncate = oper->truncate ? cache_truncate : NULL;
	cache_oper->unlink   = oper->unlink ? cache_unlink : NULL;
	cache_oper->utimens  = oper->utimens ? cache_utimens : NULL;
	cache_oper->write    = oper->write ? cache_write : NULL;
#if defined(__APPLE__) && defined(SETATTR_WANTS_MODE)
	cache_oper->setattr_x = oper->setattr_x ? cache_setattr_x : NULL;
	cache_oper->fsetattr_x = oper->fsetattr_x ? cache_fsetattr_x : NULL;
#endif
}

struct fuse_operations *cache_wrap(struct fuse_operations *oper)
{
	static struct fuse_operations cache_oper;
	cache.next_oper = oper;

	cache_fill(oper, &cache_oper);
	pthread_mutex_init(&cache.lock, NULL);
	cache.table = g_hash_table_new_full(g_str_hash, g_str_equal,
					    g_free, free_node);
	if (cache.table == NULL) {
		fprintf(stderr, "failed to create cache\n");
		return NULL;
	}
	return &cache_oper;
}

static const struct fuse_opt cache_opts[] = {
	{ "dcache_timeout=%u", offsetof(struct cache, stat_timeout_secs), 0 },
	{ "dcache_timeout=%u", offsetof(struct cache, dir_timeout_secs), 0 },
	{ "dcache_timeout=%u", offsetof(struct cache, link_timeout_secs), 0 },
	{ "dcache_stat_timeout=%u", offsetof(struct cache, stat_timeout_secs), 0 },
	{ "dcache_dir_timeout=%u", offsetof(struct cache, dir_timeout_secs), 0 },
	{ "dcache_link_timeout=%u", offsetof(struct cache, link_timeout_secs), 0 },
	{ "dcache_max_size=%u", offsetof(struct cache, max_size), 0 },
	{ "dcache_clean_interval=%u", offsetof(struct cache,
					       clean_interval_secs), 0 },
	{ "dcache_min_clean_interval=%u", offsetof(struct cache,
						   min_clean_interval_secs), 0 },

	/* For backwards compatibility */
	{ "cache_timeout=%u", offsetof(struct cache, stat_timeout_secs), 0 },
	{ "cache_timeout=%u", offsetof(struct cache, dir_timeout_secs), 0 },
	{ "cache_timeout=%u", offsetof(struct cache, link_timeout_secs), 0 },
	{ "cache_stat_timeout=%u", offsetof(struct cache, stat_timeout_secs), 0 },
	{ "cache_dir_timeout=%u", offsetof(struct cache, dir_timeout_secs), 0 },
	{ "cache_link_timeout=%u", offsetof(struct cache, link_timeout_secs), 0 },
	{ "cache_max_size=%u", offsetof(struct cache, max_size), 0 },
	{ "cache_clean_interval=%u", offsetof(struct cache,
					       clean_interval_secs), 0 },
	{ "cache_min_clean_interval=%u", offsetof(struct cache,
						   min_clean_interval_secs), 0 },
	FUSE_OPT_END
};

int cache_parse_options(struct fuse_args *args)
{
	cache.stat_timeout_secs = DEFAULT_CACHE_TIMEOUT_SECS;
	cache.dir_timeout_secs = DEFAULT_CACHE_TIMEOUT_SECS;
	cache.link_timeout_secs = DEFAULT_CACHE_TIMEOUT_SECS;
	cache.max_size = DEFAULT_MAX_CACHE_SIZE;
	cache.clean_interval_secs = DEFAULT_CACHE_CLEAN_INTERVAL_SECS;
	cache.min_clean_interval_secs = DEFAULT_MIN_CACHE_CLEAN_INTERVAL_SECS;

	return fuse_opt_parse(args, &cache, cache_opts, NULL);
}
