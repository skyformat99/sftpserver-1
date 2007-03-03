#include "sftpserver.h"
#include "types.h"
#include "users.h"
#include "sftp.h"
#include "alloc.h"
#include "debug.h"
#include "stat.h"
#include <sys/stat.h>
#include <string.h>
#include <stdio.h>
#include <time.h>
#include <sys/time.h>
#include <unistd.h>

void stat_to_attrs(struct allocator *a,
		   const struct stat *sb, struct sftpattr *attrs,
                   uint32_t flags) {
  memset(attrs, 0, sizeof *attrs);
  attrs->valid = (SSH_FILEXFER_ATTR_SIZE
		  |SSH_FILEXFER_ATTR_PERMISSIONS
		  |SSH_FILEXFER_ATTR_ACCESSTIME
		  |SSH_FILEXFER_ATTR_CREATETIME
		  |SSH_FILEXFER_ATTR_MODIFYTIME
		  |SSH_FILEXFER_ATTR_UIDGID
		  |SSH_FILEXFER_ATTR_OWNERGROUP
		  |SSH_FILEXFER_ATTR_ALLOCATION_SIZE
		  |SSH_FILEXFER_ATTR_LINK_COUNT
		  |SSH_FILEXFER_ATTR_CTIME);
  switch(sb->st_mode & S_IFMT) {
  case S_IFIFO: attrs->type = SSH_FILEXFER_TYPE_FIFO; break;
  case S_IFCHR: attrs->type = SSH_FILEXFER_TYPE_CHAR_DEVICE; break;
  case S_IFDIR: attrs->type = SSH_FILEXFER_TYPE_DIRECTORY; break;
  case S_IFBLK: attrs->type = SSH_FILEXFER_TYPE_BLOCK_DEVICE; break;
  case S_IFREG: attrs->type = SSH_FILEXFER_TYPE_REGULAR; break;
  case S_IFLNK: attrs->type = SSH_FILEXFER_TYPE_SYMLINK; break;
  case S_IFSOCK: attrs->type = SSH_FILEXFER_TYPE_SOCKET; break;
  default: attrs->type = SSH_FILEXFER_TYPE_SPECIAL; break;
  }
  attrs->size = sb->st_size;
  attrs->allocation_size = sb->st_blksize;
  /* Only look up owner/group info if wanted */
  if(flags & SSH_FILEXFER_ATTR_OWNERGROUP) {
    attrs->owner = uid2name(a, sb->st_uid);
    attrs->group = gid2name(a, sb->st_gid);
  } else {
    attrs->valid &= ~SSH_FILEXFER_ATTR_OWNERGROUP;
  }
  attrs->uid = sb->st_uid;
  attrs->gid = sb->st_gid;
  attrs->permissions = sb->st_mode;
  attrs->atime.seconds = sb->st_atime;
  attrs->mtime.seconds = sb->st_mtime;
  attrs->ctime.seconds = sb->st_ctime;
#if HAVE_STAT_TIMESPEC
  attrs->atime.nanoseconds = 1000 * sb->st_atimespec.tv_nsec;
  attrs->mtime.nanoseconds = 1000 * sb->st_mtimespec.tv_nsec;
  attrs->ctime.nanoseconds = 1000 * sb->st_ctimespec.tv_nsec;
  attrs->valid |= SSH_FILEXFER_ATTR_SUBSECOND_TIMES;
#endif
  attrs->link_count = sb->st_nlink;
}

const char *format_attr(struct allocator *a,
			const struct sftpattr *attrs, int thisyear,
			unsigned long flags) {
  char perms[64], linkcount[64], size[64], date[64], nowner[64], ngroup[64];
  char *formatted, *p;
  const char *owner, *group;
  static const char typedetails[] = "?-dl??scbp";

  /* permissions */
  p = perms;
  *p++ = typedetails[attrs->type];
  if(attrs->valid & SSH_FILEXFER_ATTR_PERMISSIONS) {
    *p++ = (attrs->permissions & 00400) ? 'r' : '-';
    *p++ = (attrs->permissions & 00200) ? 'w' : '-';
    switch(attrs->permissions & 04100) {
    case 00000: *p++ = '-'; break;
    case 00100: *p++ = 'x'; break;
    case 04000: *p++ = 'S'; break;
    case 04100: *p++ = 's'; break;
    }
    *p++ = (attrs->permissions & 00040) ? 'r' : '-';
    *p++ = (attrs->permissions & 00020) ? 'w' : '-';
    switch(attrs->permissions & 02010) {
    case 00000: *p++ = '-'; break;
    case 00010: *p++ = 'x'; break;
    case 02000: *p++ = 'S'; break;
    case 02010: *p++ = 's'; break;
    }
    *p++ = (attrs->permissions & 00004) ? 'r' : '-';
    *p++ = (attrs->permissions & 00002) ? 'w' : '-';
    switch(attrs->permissions & 01001) {
    case 00000: *p++ = '-'; break;
    case 00001: *p++ = 'x'; break;
    case 01000: *p++ = 'T'; break;
    case 01001: *p++ = 't'; break;
    }
    *p = 0;
  } else
    strcpy(p + 1, "?????????");
  /* link count */
  if(attrs->valid & SSH_FILEXFER_ATTR_LINK_COUNT)
    sprintf(linkcount, "%"PRIu32, attrs->link_count);
  else
    strcpy(linkcount, "?");
  /* size */
  if(attrs->valid & SSH_FILEXFER_ATTR_SIZE)
    sprintf(size, "%"PRIu64, attrs->size);
  else
    strcpy(size, "?");
  /* ownership */
  if(attrs->valid & SSH_FILEXFER_ATTR_UIDGID) {
    sprintf(nowner, "%jd", (intmax_t)attrs->uid);
    sprintf(ngroup, "%jd", (intmax_t)attrs->gid);
  }
  owner = group = "?";
  if(flags & FORMAT_PREFER_NUMERIC_UID) {
    if(attrs->valid & SSH_FILEXFER_ATTR_UIDGID) {
      owner = nowner;
      group = ngroup;
    } else if(attrs->valid & SSH_FILEXFER_ATTR_OWNERGROUP) {
      owner = attrs->owner;
      group = attrs->group;
    }
  } else {
    if(attrs->valid & SSH_FILEXFER_ATTR_OWNERGROUP) {
      owner = attrs->owner;
      group = attrs->group;
    } else if(attrs->valid & SSH_FILEXFER_ATTR_UIDGID) {
      owner = nowner;
      group = ngroup;
    }
  }
  /* timestamp */
  if(attrs->valid & SSH_FILEXFER_ATTR_MODIFYTIME) {
    struct tm mtime;
    const time_t m = attrs->mtime.seconds;
    gmtime_r(&m, &mtime);
    /* Timestamps in the current year we give down to seconds.  For
     * timestamps in other years we give the year. */
    if(mtime.tm_year == thisyear)
      strftime(date, sizeof date, "%b %d %H:%M", &mtime);
    else
      strftime(date, sizeof date, "%b %d  %Y", &mtime);
  } else
    strcpy(date, "?");
  /* Format the result */
  formatted = alloc(a, 80 + strlen(attrs->name));
  /* The draft is pretty specific about field widths */
  sprintf(formatted, "%10.10s %3.3s %-8.8s %-8.8s %8.8s %12.12s %s",
	  perms, linkcount, owner, group,
	  size, date, attrs->name);
  return formatted;
}

/* Horrendous ugliness for SETSTAT/FSETSTAT */
#if HAVE_STAT_TIMESPEC
#define SET_STATUS_NANOSEC do {                                         \
    times[0].tv_usec = ((attrs->valid & SSH_FILEXFER_ATTR_ACCESSTIME)   \
                        ? (long)attrs->atime.nanoseconds                \
                        : current.st_atimespec.tv_nsec) / 1000;         \
    times[1].tv_usec = ((attrs->valid & SSH_FILEXFER_ATTR_MODIFYTIME)   \
                        ? (long)attrs->mtime.nanoseconds                \
                        : current.st_mtimespec.tv_nsec) / 1000;         \
} while(0)
#else
#define SET_STATUS_NANOSEC ((void)0)
#endif

#define SET_STATUS(WHAT, TRUNCATE, CHOWN, CHMOD, STAT, UTIMES) do {     \
  struct timeval times[2];                                              \
  struct stat current;                                                  \
                                                                        \
  if(attrs->valid & SSH_FILEXFER_ATTR_SIZE) {                           \
    D(("...truncate to %"PRIu64, attrs->size));                         \
    if(TRUNCATE(WHAT, attrs->size) < 0)                                 \
      return #TRUNCATE;                                                 \
  }                                                                     \
  if(attrs->valid & SSH_FILEXFER_ATTR_UIDGID) {                         \
    D(("...chown to %"PRId32"/%"PRId32, attrs->uid, attrs->gid));       \
    if(CHOWN(WHAT, attrs->uid, attrs->gid) < 0)  			\
      return #CHOWN;							\
  }                                                                     \
  if(attrs->valid & SSH_FILEXFER_ATTR_PERMISSIONS) {                    \
    const mode_t mode = attrs->permissions & 0777;                      \
    D(("...chmod to %#o", (unsigned)mode));                             \
    if(CHMOD(WHAT, mode) < 0)                                           \
      return #CHMOD;                                                    \
  }                                                                     \
  if(attrs->valid & (SSH_FILEXFER_ATTR_ACCESSTIME                       \
                     |SSH_FILEXFER_ATTR_MODIFYTIME)) {                  \
    if(STAT(WHAT, &current) < 0) {                                      \
      D(("cannot stat"));                                               \
      return #STAT;                                                     \
    }                                                                   \
    times[0].tv_sec = ((attrs->valid & SSH_FILEXFER_ATTR_ACCESSTIME)    \
                       ? (time_t)attrs->atime.seconds                   \
                       : current.st_atime);                             \
    times[1].tv_sec = ((attrs->valid & SSH_FILEXFER_ATTR_MODIFYTIME)    \
                       ? (time_t)attrs->mtime.seconds                   \
                       : current.st_mtime);                             \
    SET_STATUS_NANOSEC;							\
    D(("...utimes to atime %lu.%06lu mtime %lu.%06lu",                  \
       (unsigned long)times[0].tv_sec,                                  \
       (unsigned long)times[0].tv_usec,                                 \
       (unsigned long)times[1].tv_sec,                                  \
       (unsigned long)times[1].tv_usec));                               \
    if(UTIMES(WHAT, times) < 0)                                         \
      return #UTIMES;                                                   \
  }                                                                     \
  return 0;                                                             \
} while(0)

const char *set_status(const char *path,
		       const struct sftpattr *attrs) {
  SET_STATUS(path, truncate, lchown, chmod, lstat, utimes);
}

const char *set_fstatus(int fd,
			const struct sftpattr *attrs) {
  SET_STATUS(fd, ftruncate, fchown, fchmod, fstat, futimes);
}

/*
Local Variables:
c-basic-offset:2
comment-column:40
fill-column:79
indent-tabs-mode:nil
End:
*/