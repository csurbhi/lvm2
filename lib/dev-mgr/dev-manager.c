/*
 * tools/lib/dev-manager.c
 *
 * Copyright (C) 2001 Sistina Software
 *
 * This LVM library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This LVM library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this LVM library; if not, write to the Free
 * Software Foundation, Inc., 59 Temple Place - Suite 330, Boston,
 * MA 02111-1307, USA
 */

/*
 * Changelog
 *
 *	14/08/2001 - Initial checkin (AJL)
 *	15/08/2001 - Several structural changes
 *		+ Got rid of _hash_name_lookup and _hash_dev_lookup
 *		+ Got rid of _destroy_hash_table
 *		+ The fin_* fxns now return void
 *		+ Split _hash_insert into _name_insert, _dev_insert, and
 *		  _list_insert.  Most of the logic from _hash_insert is now in
 *		  _add.
 *	20/08/2001 - Created _add_named_device and used it in dev_by_name
 *	21/08/2001 - Basic config file support added
 *
 */

#include <dirent.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <linux/kdev_t.h>
#include "hash.h"
#include "mm/pool.h"
#include "log/log.h"
#include "dev-manager.h"

#define DEFAULT_BASE_DIR "/dev"
#define LOCAL_EPARAM 1
#define LOCAL_CHECK_NAME 2
#define LOCAL_DEVICE_TYPE_INVALID 3

static const char *device_names[] = {
		"ide",			/* IDE disk */
		"sd",			/* SCSI disk */
		"md",			/* Multiple Disk driver (SoftRAID) */
		"loop",			/* Loop device */
		"dasd",			/* DASD disk (IBM S/390, zSeries) */
		"dac960",		/* DAC960 */
		"nbd",			/* Network Block Device */
		"ida",			/* Compaq SMART2 */
		"cciss",		/* Compaq CCISS array */
		"ubd",			/* User-mode virtual block device */
		NULL
};

struct dev_i {
	struct device d;

	/* Begin internal data */
	struct dev_i *dev_next;
	struct dev_i *name_next;
	struct dev_i *next;
};

/* temporary structure - will replace with something smarter later */
struct dev_n {
	int major;
	struct dev_n *next;
};

struct dev_mgr {
	/* everything is allocated from this pool */
	struct pool *pool;

	int has_scanned;

	/* linked list of valid major numbers */
	struct dev_n *dev_list;

	/* hash table */
	int num_slots;
	int mask;
	struct dev_i **dev_slots;
	struct dev_i **name_slots;
	struct dev_i *all;

	char **devdir;
};


static int _full_dev_scan(struct dev_mgr *dm);
static int _dir_scan(struct dev_mgr *dm, const char *dirname);

static struct dev_i *_add(struct dev_mgr *dm, const char *directory,
			  const char *devname);

static struct dev_i *_add_named_device(struct dev_mgr *dm, const char *name);

static struct dev_i *
_is_name_present(struct dev_mgr *dm, const char *name);

static struct dev_i *
_is_device_present(struct dev_mgr *dm, dev_t dev);

static void _collapse_slashes(char *str);
static int _create_hash_table(struct dev_mgr *dm, unsigned size_hint);
static void _name_insert(struct dev_mgr *dm, struct dev_i *device);
static void _dev_insert(struct dev_mgr *dm, struct dev_i *device);
static void _list_insert(struct dev_mgr *dm, struct dev_i *device);
static unsigned int _hash_dev(dev_t d);
static int _check_dev(struct dev_mgr *dm, struct stat *stat_b);
static int _scan_proc_dev(struct dev_mgr *dm);

static inline struct device *_get_dev(struct dev_i *di)
{
	return di ? &di->d : 0;
}

struct dev_mgr *init_dev_manager(struct config_node *cn)
{
	struct pool *pool = create_pool(10 * 1024);
	struct dev_mgr *dm = NULL;
	const char * base_dir = NULL;

	if (!pool) {
		stack;
		return 0;
	}

	if (!(dm = pool_alloc(pool, sizeof(*dm)))) {
		stack;
		destroy_pool(pool);
		return 0;
	}

	memset(dm, 0, sizeof(*dm));
	dm->pool = pool;

	if(cn) 
		base_dir = find_config_str(cn, "dev-mgr/base_dir", '/', 0);
	if(!base_dir)	
		base_dir = DEFAULT_BASE_DIR;

	dm->devdir = pool_alloc(dm->pool, sizeof(char*));
	dm->devdir[0] = pool_strdup(dm->pool, base_dir);

	if (!_create_hash_table(dm, 128)) {
		stack;
		destroy_pool(pool);
		return 0;
	}

	_scan_proc_dev(dm);

	return dm;
}

void fin_dev_manager(struct dev_mgr *dm)
{
	if(dm)
		destroy_pool(dm->pool);
}

/* Find a matching name in the hash table */
struct device *dev_by_name(struct dev_mgr *dm, const char *name)
{
	struct dev_i *c;

	if (!(c = _is_name_present(dm, name)) && !dm->has_scanned)
		c = _add_named_device(dm, name);

	return _get_dev(c);
}

/* Find a matching dev_t entry in the hash table - this could potentially
 * cause problems when there are symlinks in the cache that match up to
 * other entries... */
struct device *dev_by_dev(struct dev_mgr *dm, dev_t d)
{
	return _get_dev(_is_device_present(dm, d));
}

/* Triggers a full scan the first time it is run, and returns a counter
   object*/
dev_counter_t init_dev_scan(struct dev_mgr *dm)
{
	if(!dm->has_scanned)
		_full_dev_scan(dm);

	return (dev_counter_t) dm->all;
}

/* Returns the next item in the device cache */
struct device *next_device(dev_counter_t *counter)
{
	struct dev_i *di = (struct dev_i *) *counter;
	if (di)
		*counter = (dev_counter_t) di->next;

	return  _get_dev(di);
}

/* Cleans up the counter object...doesn't need to do anything with the current
   implementation */
void fin_dev_scan(dev_counter_t counter)
{
	/* empty */
}

/* Scan through all the directories specified (eventually a var in the config
 * file) and add all devices to the cache that we find in them */
static int _full_dev_scan(struct dev_mgr *dm)
{
	int d, ret = 0;
	char *dirname;

	for (d = 0; dm->devdir[d]; d++) {
		dirname = dm->devdir[d];
		ret = ret || _dir_scan(dm, dirname);
	}

	dm->has_scanned = 1;
	return ret;
}

/* Scan the directory passed in and add the entries found to the hash */
static int _dir_scan(struct dev_mgr *dm, const char *dirname)
{
	int n, dirent_count;
	struct dirent **dirent;

	dirent_count = scandir(dirname, &dirent, NULL, alphasort);
	if (dirent_count > 0) {
		for (n = 0; n < dirent_count; n++) {
			_add(dm, dirname, dirent[n]->d_name);
			free(dirent[n]);
		}

		free(dirent);
	}

	return 1;
}

/* Add entry found to hash if it is a device; if it is a directory, call
 * _dir_scan with it as the argument */
static struct dev_i *_add(struct dev_mgr *dm,
			  const char *directory, const char *devname)
{
	char devpath[128];

	if (!directory || !devname)
		return 0;

	snprintf(devpath, sizeof(devpath), "%s/%s", directory, devname);
	_collapse_slashes(devpath);
	return _add_named_device(dm, devpath);
}

static struct dev_i *_add_named_device(struct dev_mgr *dm, const char *devpath)
{
	struct dev_i *dev = NULL;
	struct stat stat_b;

	/* FIXME: move lvm_check_dev into this file */
	if ((stat(devpath, &stat_b) == -1) || _check_dev(dm, &stat_b))
		goto out;

	/* Check for directories and scan them if they aren't this directory
	   or a parent of the directory */
	if (S_ISDIR(stat_b.st_mode)) {
		if(devpath[0] != '.')
			_dir_scan(dm, devpath);

	} else {
		/* If the device is in the name hash, we just need to update
		   the device type (and it's already in the device hash and
		   list */
		if((dev = _is_name_present(dm, devpath)))
			dev->d.dev = stat_b.st_rdev;

		/* otherwise we need to add it to the name hash and possibly
		   the device hash and list */
		else {
			if (!(dev = pool_alloc(dm->pool, sizeof(*dev)))) {
				stack;
				goto out;
			}

			if (!(dev->d.name = pool_strdup(dm->pool, devpath))) {
				stack;
				goto out;
			}

			dev->d.dev = stat_b.st_rdev;

			/* We don't care what the name is as long as it is
			   a valid device - this allows us to deal with
			   symlinks properly */
			_name_insert(dm, dev);

			/* the device type hash and list have to have unique
			   entries (based on device type) so we can be assured
			   of only one hit when searching for a device and
			   we don't get duplicates when scanning the list */
			if(!_is_device_present(dm, dev->d.dev)) {
				_dev_insert(dm, dev);
				_list_insert(dm, dev);
			}
		}
	}

 out:
	log_info(dev ? "dev-manager added '%s'" :
		       "dev-manager failed to add '%s'", devpath);
	return dev;
}

/* Check to see if the name is stored in the hash table already */
static struct dev_i *
_is_name_present(struct dev_mgr *dm, const char *name)
{
      	unsigned h = hash(name) & (dm->num_slots - 1);
	struct dev_i *c;

	for(c = dm->name_slots[h]; c; c = c->name_next)
		if(!strcmp(name, c->d.name))
			break;

	return c;
}

/* Check to see if dev is stored in the hash table already */
static struct dev_i *
_is_device_present(struct dev_mgr *dm, dev_t dev)
{
	unsigned h = _hash_dev(dev) & (dm->num_slots - 1);
	struct dev_i *c;

	for(c = dm->dev_slots[h]; c; c = c->dev_next)
		if(dev == c->d.dev)
			break;

	return c;
}

/* Get rid of extra slashes in the path string */
static void _collapse_slashes(char *str)
{
	char *ptr;
	int was_slash = 0;

	for (ptr = str; *ptr; ptr++) {
		if (*ptr == '/') {
			if (was_slash)
				continue;

			was_slash = 1;
		} else
			was_slash = 0;
		*str++ = *ptr;
	}

	*str = *ptr;
}

static int _create_hash_table(struct dev_mgr *dm, unsigned size_hint)
{
	size_t len;
	unsigned new_size = 16u;

	/* round size hint up to a power of two */
	while(new_size < size_hint)
		new_size = new_size << 1;

	dm->num_slots = new_size;
	dm->mask = new_size - 1;

	len = sizeof(*dm->dev_slots) * new_size;
	if (!(dm->dev_slots = pool_zalloc(dm->pool, len)))
		return 0;

	if (!(dm->name_slots = pool_zalloc(dm->pool, len)))
		return 0;

	return 1;
}

static void _name_insert(struct dev_mgr *dm, struct dev_i *device)
{
	unsigned h = hash(device->d.name) & dm->mask;

	/* look for this key */
	struct dev_i *c = _is_name_present(dm, device->d.name);

	/*FIXME: We might not want to replace the entry if a duplicate is
	  found...For now just replace the device type entry - what does
	  this mean for freeing memory...? */
	if (c)
		c->d.dev = device->d.dev;

	else {
		device->name_next = dm->name_slots[h];
		dm->name_slots[h] = device;
	}
}

static void _dev_insert(struct dev_mgr *dm, struct dev_i *device)
{
	unsigned h = _hash_dev(device->d.dev) & dm->mask;

	device->dev_next = dm->dev_slots[h];
	dm->dev_slots[h] = device;
}

static void _list_insert(struct dev_mgr *dm, struct dev_i *device)
{
	/* Handle the list of devices */
        device->next = dm->all;
        dm->all = device;
}

static unsigned int _hash_dev(dev_t d)
{
	/* FIXME: find suitable fn from Knuth */
	return (unsigned int) d;
}

static int _check_dev(struct dev_mgr *dm, struct stat *stat_b)
{
	int ret = 1;
	int seek_major = MAJOR(stat_b->st_rdev);
	struct dev_n *node;

	if (stat_b == NULL)
		ret = -LOCAL_EPARAM;
	else if ( ! S_ISBLK(stat_b->st_mode))
		ret = -LOCAL_CHECK_NAME;
	else {
		for(node = dm->dev_list; node != NULL; node = node->next) 
			if (node->major == seek_major)
				ret = 0;
	}

	return ret;
}

/* Caches /proc/device info and 
   returns the number of block devices found in /proc/devices */
static int _scan_proc_dev(struct dev_mgr *dm)
{
	char line[80];
	FILE *procdevices = NULL;
	int ret = 0;
	int i, j = 0;
	int line_major = 0;
	int blocksection = 0;
	struct dev_n * dev_node= NULL;

	if ((procdevices = fopen("/proc/devices", "r")) != NULL) {
		while (fgets(line, 80, procdevices) != NULL) {
			i = 0;
			while (line[i] == ' ' && line[i] != '\0')
				i++;
			
			/* If its not a number it may be name of section */
			line_major = atoi(((char *) (line + i)));
			if (line_major == 0) {
				blocksection = (line[i] == 'B') ? 1 : 0;
				continue;
			}
			/* We only want block devices ... */
			if (!blocksection)
				continue;
			
			/* Find the start of the device major name */
			while (line[i] != ' ' && line[i] != '\0')
				i++;
			while (line[i] == ' ' && line[i] != '\0')
				i++;

			for (j = 0; device_names[j] != NULL; j++) {
				if (strlen(device_names[j])
						<= strlen(line + i)) {
					if (strncmp (device_names[j],
					    line + i,
					    strlen(device_names[j])) == 0) {
						dev_node = pool_alloc(dm->pool,
							sizeof(*dev_node));
						dev_node->major = line_major;
						dev_node->next=dm->dev_list;
						dm->dev_list = dev_node;
						ret++;
						break;
					}
				}
			}
		}
		fclose(procdevices);
	}
	return ret;
}

/*
 * Local variables:
 * c-file-style: "linux"
 * End:
 * vim:ai cin ts=8
 */

