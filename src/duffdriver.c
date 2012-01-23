/*
 * duff - Duplicate file finder
 * Copyright (c) 2005 Camilla Berglund <elmindreda@elmindreda.org>
 *
 * This software is provided 'as-is', without any express or implied
 * warranty. In no event will the authors be held liable for any
 * damages arising from the use of this software.
 *
 * Permission is granted to anyone to use this software for any
 * purpose, including commercial applications, and to alter it and
 * redistribute it freely, subject to the following restrictions:
 *
 *  1. The origin of this software must not be misrepresented; you
 *     must not claim that you wrote the original software. If you use
 *     this software in a product, an acknowledgment in the product
 *     documentation would be appreciated but is not required.
 *
 *  2. Altered source versions must be plainly marked as such, and
 *     must not be misrepresented as being the original software.
 *
 *  3. This notice may not be removed or altered from any source
 *     distribution.
 */

#if HAVE_CONFIG_H
#include "config.h"
#endif

#if HAVE_SYS_TYPES_H
#include <sys/types.h>
#endif

#if HAVE_SYS_STAT_H
#include <sys/stat.h>
#endif

#if HAVE_INTTYPES_H
#include <inttypes.h>
#elif HAVE_STDINT_H
#include <stdint.h>
#endif

#if HAVE_ERRNO_H
#include <errno.h>
#endif

#if HAVE_UNISTD_H
#include <unistd.h>
#endif

#if HAVE_STDIO_H
#include <stdio.h>
#endif

#if HAVE_STRING_H
#include <string.h>
#endif

#if HAVE_STDARG_H
#include <stdarg.h>
#endif

#if HAVE_STDLIB_H
#include <stdlib.h>
#endif

#if HAVE_DIRENT_H
  #include <dirent.h>
  #define NAMLEN(dirent) strlen((dirent)->d_name)
#else
  #define dirent direct
  #define NAMLEN(dirent) (dirent)->d_namlen
#if HAVE_SYS_NDIR_H
  #include <sys/ndir.h>
  #endif
    #if HAVE_SYS_DIR_H
      #include <sys/dir.h>
    #endif
  #if HAVE_NDIR_H
    #include <ndir.h>
  #endif
#endif

#include "duffstring.h"
#include "duff.h"

#define BUCKET_COUNT (1 << HASH_BITS)
#define BUCKET_INDEX(size) ((size) & (BUCKET_COUNT - 1))

/* These flags are defined and documented in duff.c.
 */
extern int follow_links_mode;
extern int all_files_flag;
extern int null_terminate_flag;
extern int recursive_flag;
extern int ignore_empty_flag;
extern int quiet_flag;
extern int physical_flag;
extern int excess_flag;
extern const char* header_format;
extern int header_uses_digest;

/* Buckets of list of collected entries.
 */
static List buckets[1 << HASH_BITS];
/* List head for traversed directories.
 */
static Directory* directories = NULL;

/* These functions are documented below, where they are defined.
 */
static int stat_path(const char* path, struct stat* sb, int depth);
static int has_recursed_directory(dev_t device, ino_t inode);
static void recurse_directory(const char* path,
                              const struct stat* sb,
			      int depth);
static void process_file(const char* path, struct stat* sb);
static void process_path(const char* path, int depth);
static void report_cluster(List* duplicates, unsigned int index);
static void report_clusters(void);

/* Stat:s a file according to the specified options.
 */
static int stat_path(const char* path, struct stat* sb, int depth)
{
  if (*path == '\0')
    return -1;

  if (lstat(path, sb) != 0)
  {
    if (!quiet_flag)
      warning("%s: %s", path, strerror(errno));

    return -1;
  }

  if (S_ISLNK(sb->st_mode))
  {
    if (follow_links_mode == ALL_SYMLINKS ||
        (depth == 0 && follow_links_mode == ARG_SYMLINKS))
    {
      if (stat(path, sb) != 0)
      {
	if (!quiet_flag)
	  warning("%s: %s", path, strerror(errno));

	return -1;
      }

      if (S_ISDIR(sb->st_mode))
	return -1;
    }
    else
      return -1;
  }

  return 0;
}

/* Returns true if the directory has already been recursed into.
 * TODO: Implement a more efficient data structure.
 */
static int has_recursed_directory(dev_t device, ino_t inode)
{
  Directory* dir;

  for (dir = directories;  dir;  dir = dir->next)
  {
    if (dir->device == device && dir->inode == inode)
      return 1;
  }

  return 0;
}

/* Records the specified directory as recursed.
 * TODO: Implement a more efficient data structure.
 */
static void record_directory(dev_t device, ino_t inode)
{
  Directory* dir;

  dir = (Directory*) malloc(sizeof(Directory));
  dir->device = device;
  dir->inode = inode;
  dir->next = directories;
  directories = dir;
}

/* Recurses into a directory, collecting all or all non-hidden files,
 * according to the specified options.
 */
static void recurse_directory(const char* path,
                              const struct stat* sb,
			      int depth)
{
  DIR* dir;
  struct dirent* dir_entry;
  char* child_path;
  const char* name;

  if (has_recursed_directory(sb->st_dev, sb->st_ino))
    return;

  record_directory(sb->st_dev, sb->st_ino);

  dir = opendir(path);
  if (!dir)
  {
    if (!quiet_flag)
      warning("%s: %s", path, strerror(errno));

    return;
  }

  while ((dir_entry = readdir(dir)))
  {
    name = dir_entry->d_name;
    if (name[0] == '.')
    {
      if (!all_files_flag)
	continue;

      if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0)
	continue;
    }

    if (asprintf(&child_path, "%s/%s", path, name) > 0)
    {
      process_path(child_path, depth);
      free(child_path);
    }
  }

  closedir(dir);
}

/* Processes a single file.
 */
static void process_file(const char* path, struct stat* sb)
{
  Entry* entry;

  if (sb->st_size == 0)
  {
    if (ignore_empty_flag)
      return;
  }
  else
  {
    if (access(path, R_OK) != 0)
    {
      /* We can't read the file, so we fail here */

      if (!quiet_flag)
        warning("%s: %s", path, strerror(errno));

      return;
    }
  }

  /* NOTE: Check for duplicate arguments? */

  if (physical_flag)
  {
    /* TODO: Make this less pessimal */

    size_t i, bucket = BUCKET_INDEX(sb->st_size);

    for (i = 0;  i < buckets[bucket].allocated;  i++)
    {
      if (buckets[bucket].entries[i].device == sb->st_dev &&
          buckets[bucket].entries[i].inode == sb->st_ino)
      {
        return;
      }
    }
  }

  fill_entry(entry_list_alloc(&buckets[BUCKET_INDEX(sb->st_size)]), path, sb);
}

/* Initializes the driver, processes the specified arguments and reports the
 * clusters found.
 */
void process_args(int argc, char** argv)
{
  size_t i, j;
  char path[PATH_MAX];

  for (i = 0;  i < BUCKET_COUNT;  i++)
    entry_list_init(&buckets[i]);

  if (argc)
  {
    /* Read file names from command line */
    for (i = 0;  i < argc;  i++)
    {
      kill_trailing_slashes(argv[i]);
      process_path(argv[i], 0);
    }
  }
  else
  {
    /* Read file names from stdin */
    while (read_path(stdin, path, sizeof(path)) == 0)
    {
      kill_trailing_slashes(path);
      process_path(path, 0);
    }
  }

  report_clusters();

  for (i = 0;  i < BUCKET_COUNT;  i++)
  {
    for (j = 0;  j < buckets[i].allocated;  j++)
      free_entry(&buckets[i].entries[j]);

    entry_list_free(&buckets[i]);
  }
}

/* Processes a path name, whether from the command line or from
 * directory recursion, according to its type.
 */
void process_path(const char* path, int depth)
{
  mode_t mode;
  struct stat sb;

  if (stat_path(path, &sb, depth) != 0)
    return;

  mode = sb.st_mode & S_IFMT;
  switch (mode)
  {
    case S_IFREG:
    {
      process_file(path, &sb);
      break;
    }

    case S_IFDIR:
    {
      if (recursive_flag)
      {
	recurse_directory(path, &sb, depth + 1);
        break;
      }

      /* FALLTHROUGH */
    }

    default:
    {
      if (!quiet_flag)
      {
	switch (mode)
	{
	  case S_IFLNK:
	    warning(_("%s is a symbolic link; skipping"), path);
	    break;
	  case S_IFIFO:
	    warning(_("%s is a named pipe; skipping"), path);
	    break;
          case S_IFBLK:
	    warning(_("%s is a block device; skipping"), path);
	    break;
	  case S_IFCHR:
	    warning(_("%s is a character device; skipping"), path);
	    break;
	  case S_IFDIR:
	    warning(_("%s is a directory; skipping"), path);
	    break;
	  case S_IFSOCK:
	    warning(_("%s is a socket; skipping"), path);
	    break;
	  default:
	    error(_("This cannot happen"));
	}
      }
    }
  }
}

/* Reports a cluster to stdout, according to the specified options.
 */
static void report_cluster(List* duplicates, unsigned int index)
{
  size_t i;
  Entry* entries = duplicates->entries;

  if (excess_flag)
  {
    entries[0].status = REPORTED;

    /* Report all but the first entry in the cluster */
    for (i = 1;  i < duplicates->allocated;  i++)
    {
      if (null_terminate_flag)
      {
        fputs(entries[i].path, stdout);
        fputc('\0', stdout);
      }
      else
        printf("%s\n", entries[i].path);

      entries[i].status = REPORTED;
    }
  }
  else
  {
    /* Print header and report all entries in the cluster */

    if (*header_format != '\0')
    {
      if (header_uses_digest)
        generate_entry_digest(entries);

      print_cluster_header(header_format,
			   duplicates->allocated,
			   index,
			   entries->size,
			   entries->digest);

      if (null_terminate_flag)
	fputc('\0', stdout);
      else
	fputc('\n', stdout);
    }

    for (i = 0;  i < duplicates->allocated;  i++)
    {
      if (null_terminate_flag)
      {
	fputs(entries[i].path, stdout);
	fputc('\0', stdout);
      }
      else
	printf("%s\n", entries[i].path);

      entries[i].status = REPORTED;
    }
  }
}

/* Finds and reports all duplicate clusters among the collected entries.
 */
static void report_clusters(void)
{
  size_t i, first, second, index;
  List duplicates;

  entry_list_init(&duplicates);

  for (i = 0;  i < BUCKET_COUNT;  i++)
  {
    for (first = 0;  first < buckets[i].allocated;  first++)
    {
      if (buckets[i].entries[first].status == INVALID ||
          buckets[i].entries[first].status == REPORTED)
      {
        continue;
      }

      for (second = first + 1;  second < buckets[i].allocated;  second++)
      {
        if (buckets[i].entries[second].status == INVALID ||
            buckets[i].entries[second].status == REPORTED)
        {
            continue;
        }

        if (compare_entries(&buckets[i].entries[first],
                            &buckets[i].entries[second]) == 0)
        {
          if (duplicates.allocated == 0)
            *entry_list_alloc(&duplicates) = buckets[i].entries[first];

          *entry_list_alloc(&duplicates) = buckets[i].entries[second];
        }
      }

      if (duplicates.allocated)
      {
        report_cluster(&duplicates, index);
        entry_list_empty(&duplicates);

        index++;
      }
    }
  }

  entry_list_free(&duplicates);
}

