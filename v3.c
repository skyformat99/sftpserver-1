#include "sftpserver.h"
#include "alloc.h"
#include "users.h"
#include "debug.h"
#include "sftp.h"
#include "handle.h"
#include "send.h"
#include "parse.h"
#include "types.h"
#include "globals.h"
#include "stat.h"
#include "utils.h"
#include "serialize.h"
#include <errno.h>
#include <string.h>
#include <stdlib.h>
#include <limits.h>
#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#include <sys/stat.h>
#include <sys/time.h>

/* Callbacks */

/* Encode/decode path names.  v3 does not know what encoding filenames use.  We
 * assume that the client and the server use the same encoding and so don't
 * perform any translation. */
static int v3_encode(struct sftpjob attribute((unused)) *job,
                     char attribute((unused)) **path) {
  return 0;
}

static int v3_decode(struct sftpjob attribute((unused)) *job, 
                     char attribute((unused)) **path) {
  return 0;
}

/* Send a filename list as found in an SSH_FXP_NAME response.  The response
 * header and so on must be generated by the caller. */
static void v3_sendnames(struct sftpjob *job, 
                         int nnames, const struct sftpattr *names) {
  time_t now;
  struct tm nowtime;

  /* We'd like to know what year we're in for dates in longname */
  time(&now);
  gmtime_r(&now, &nowtime);
  send_uint32(job, nnames);
  while(nnames > 0) {
    send_path(job, names->name);
    send_string(job, format_attr(job->a, names, nowtime.tm_year, 0));
    protocol->sendattrs(job, names);
    ++names;
    --nnames;
  }
}

static void v3_sendattrs(struct sftpjob *job,
                         const struct sftpattr *attrs) {
  uint32_t v3bits, m, a;

  /* The timestamp flags change between v3 and v4.  In the structure we always
   * use the v4+ bits, so we must translate. */
  if((attrs->valid & (SSH_FILEXFER_ATTR_ACCESSTIME
                      |SSH_FILEXFER_ATTR_MODIFYTIME))
     == (SSH_FILEXFER_ATTR_ACCESSTIME
         |SSH_FILEXFER_ATTR_MODIFYTIME))
    v3bits = ((attrs->valid & (SSH_FILEXFER_ATTR_SIZE
                               |SSH_FILEXFER_ATTR_UIDGID
                               |SSH_FILEXFER_ATTR_PERMISSIONS))
              |SSH_FILEXFER_ACMODTIME);
  else
    v3bits = (attrs->valid & (SSH_FILEXFER_ATTR_SIZE
                              |SSH_FILEXFER_ATTR_UIDGID
                              |SSH_FILEXFER_ATTR_PERMISSIONS));
  send_uint32(job, v3bits);
  if(v3bits & SSH_FILEXFER_ATTR_SIZE)
    send_uint64(job, attrs->size);
  if(v3bits & SSH_FILEXFER_ATTR_UIDGID) {
    send_uint32(job, attrs->uid);
    send_uint32(job, attrs->gid);
  }
  if(v3bits & SSH_FILEXFER_ATTR_PERMISSIONS)
    send_uint32(job, attrs->permissions);
  if(v3bits & SSH_FILEXFER_ACMODTIME) {
    m = attrs->mtime.seconds;
    a = attrs->atime.seconds;
    /* Check that the conversion was sound.  SFTP v3 becomes unsound in 2038CE.
     * If you're looking at this code then, I suggest using a later protocol
     * version.  If that's not acceptable, and you either don't care about
     * bogus timestamps or have some other workaround, then delete the
     * checks. */
    if(m != attrs->mtime.seconds)
      fatal("sending out-of-range mtime");
    if(a != attrs->atime.seconds)
      fatal("sending out-of-range mtime");
    send_uint32(job, m);
    send_uint32(job, a);
  }
  /* Note that we just discard unknown bits rather than reporting errors. */
}

static int v3_parseattrs(struct sftpjob *job, struct sftpattr *attrs) {
  uint32_t n;

  memset(attrs, 0, sizeof *attrs);
  if(parse_uint32(job, &attrs->valid)) return -1;
  /* Translate v3 bits t v4+ bits */
  if(attrs->valid & SSH_FILEXFER_ACMODTIME)
    attrs->valid |= (SSH_FILEXFER_ATTR_ACCESSTIME
                     |SSH_FILEXFER_ATTR_MODIFYTIME);
  /* Read the v3 fields */
  if(attrs->valid & SSH_FILEXFER_ATTR_SIZE)
    if(parse_uint64(job, &attrs->size)) return -1;
  if(attrs->valid & SSH_FILEXFER_ATTR_UIDGID) {
    if(parse_uint32(job, &attrs->uid)) return -1;
    if(parse_uint32(job, &attrs->gid)) return -1;
  }
  if(attrs->valid & SSH_FILEXFER_ATTR_PERMISSIONS) {
    if(parse_uint32(job, &attrs->permissions)) return -1;
    /* Fake up type field */
    switch(attrs->permissions & S_IFMT) {
    case S_IFIFO: attrs->type = SSH_FILEXFER_TYPE_FIFO; break;
    case S_IFCHR: attrs->type = SSH_FILEXFER_TYPE_CHAR_DEVICE; break;
    case S_IFDIR: attrs->type = SSH_FILEXFER_TYPE_DIRECTORY; break;
    case S_IFBLK: attrs->type = SSH_FILEXFER_TYPE_BLOCK_DEVICE; break;
    case S_IFREG: attrs->type = SSH_FILEXFER_TYPE_REGULAR; break;
    case S_IFLNK: attrs->type = SSH_FILEXFER_TYPE_SYMLINK; break;
    case S_IFSOCK: attrs->type = SSH_FILEXFER_TYPE_SOCKET; break;
    default: attrs->type = SSH_FILEXFER_TYPE_UNKNOWN; break;
    }
  } else
    attrs->type = SSH_FILEXFER_TYPE_UNKNOWN;
  if(attrs->valid & SSH_FILEXFER_ATTR_ACCESSTIME) {
    if(parse_uint32(job, &n)) return -1;
    attrs->atime.seconds = n;
  }
  if(attrs->valid & SSH_FILEXFER_ATTR_MODIFYTIME) {
    if(parse_uint32(job, &n)) return -1;
    attrs->mtime.seconds = n;
  }
  if(attrs->valid & SSH_FILEXFER_ATTR_EXTENDED) {
    if(parse_uint32(job, &n)) return -1;
    while(n-- > 0) {
      parse_string(job, 0, 0);
      parse_string(job, 0, 0);
    }
  }
  return 0;
}

/* Command implementations */

void sftp_already_init(struct sftpjob *job) {
  /* Cannot initialize more than once */
  send_status(job, SSH_FX_FAILURE, "already initialized");
}

void sftp_remove(struct sftpjob *job) {
  char *path;
  
  pcheck(parse_path(job, &path));
  D(("sftp_remove %s", path));
  if(unlink(path) < 0) send_errno_status(job);
  else send_ok(job);
}

void sftp_rmdir(struct sftpjob *job) {
  char *path;
  
  pcheck(parse_path(job, &path));
  D(("sftp_rmdir %s", path));
  if(rmdir(path) < 0) send_errno_status(job);
  else send_ok(job);
}

void sftp_v34_rename(struct sftpjob *job) {
  char *oldpath, *newpath;

  pcheck(parse_path(job, &oldpath));
  pcheck(parse_path(job, &newpath));
  D(("sftp_v34_rename %s %s", oldpath, newpath));
  /* newpath is not allowed to exist.  We enforce this atomically by attempting
     to link() from oldpath to newpath and unlinking oldpath if it succeeds. */
  if(link(oldpath, newpath) < 0) {
    if(errno != EEXIST) {
      /* newpath does not exist but something stopped us renaming.  Two
       * important cases are where oldpath is a directory, which link() cannot
       * support but the v3/v4 drafts say we should, or where both paths are on
       * a filesystem incapable of hard links.
       *
       * We give up on atomicity for such cases (v3/v4 drafts do not state a
       * requirement for it) and have other useful semantics instead.
       *
       * This has the slightly odd effect of giving rename(2) semantics only
       * for directories and on primitive filesystems.  If you want such
       * semantics reliably you need SFTP v5 or better.
       */
      if(rename(oldpath, newpath) < 0)
        send_errno_status(job);
      else
        send_ok(job);
    } else
      send_errno_status(job);
  } else if(unlink(oldpath) < 0) {
    send_errno_status(job);
    unlink(newpath);
  } else
    send_ok(job);
}

void sftp_symlink(struct sftpjob *job) {
  char *oldpath, *newpath;

  pcheck(parse_path(job, &oldpath));
  pcheck(parse_path(job, &newpath));
  D(("sftp_symlink %s %s", oldpath, newpath));
  if(symlink(oldpath, newpath) < 0) send_errno_status(job);
  else send_ok(job);
  /* v3-v5 don't specify what happens if the target exists.  We take whatever
   * the OS gives us. */
}

void sftp_readlink(struct sftpjob *job) {
  char *path;
  char *result;
  int n;
  size_t nresult;
  struct sftpattr attr;

  pcheck(parse_path(job, &path));
  D(("sftp_readlink %s", path));
  /* readlink(2) has a rather stupid interface */
  nresult = 256;
  while(nresult > 0 && nresult <= 65536) {
    result = alloc(job->a, nresult);
    n = readlink(path, result, nresult);
    if(n < 0) {
      send_errno_status(job);
      return;
    }
    if((unsigned)n < nresult) {
      /* 1-element name list */
      path[n] = 0;
      memset(&attr, 0, sizeof attr);
      attr.name = path;
      send_begin(job);
      send_uint8(job, SSH_FXP_NAME);
      send_uint32(job, job->id);
      protocol->sendnames(job, 1, &attr);
      send_end(job);
      return;
    }
    nresult *= 2;
  }
  /* We should have wasted at most about 128Kbyte if we get here */
  send_status(job, SSH_FX_FAILURE, "link name is too long");
}

void sftp_opendir(struct sftpjob *job) {
  char *path;
  DIR *dp;
  struct handleid id;

  pcheck(parse_path(job, &path));
  D(("sftp_opendir %s", path));
  if(!(dp = opendir(path))) {
    send_errno_status(job);
    return;
  }
  handle_new_dir(&id, dp, path);
  D(("...handle is %"PRIu32" %"PRIu32, id.id, id.tag));
  send_begin(job);
  send_uint8(job, SSH_FXP_HANDLE);
  send_uint32(job, job->id);
  send_handle(job, &id);
  send_end(job);
}

void sftp_readdir(struct sftpjob *job) {
  struct handleid id;
  DIR *dp;
  uint32_t rc;
  struct sftpattr d[MAXNAMES];
  int n;
  struct dirent *de;
  const char *path;
  char *childpath, *fullpath;
  struct stat sb;
  
  pcheck(parse_handle(job, &id));
  D(("sftp_readdir %"PRIu32" %"PRIu32, id.id, id.tag));
  if((rc = handle_get_dir(&id, &dp, &path))) {
    send_status(job, rc, "invalid directory handle");
    return;
  }
  memset(d, 0, sizeof d);
  for(n = 0; n < MAXNAMES;) {
    /* readdir() has a slightly shonky interface - a null return can mean EOF
     * or error, and there is no guarantee that errno is reset to 0 on EOF. */
    errno = 0;
    de = readdir(dp);
    if(!de)
      break;
    /* We include . and .. in the list - if the cliient doesn't like them it
     * can filter them out itself. */
    childpath = strcpy(alloc(job->a, strlen(de->d_name) + 1), de->d_name);
    /* We need the full path to be able to stat the file */
    fullpath = alloc(job->a, strlen(path) + strlen(childpath) + 2);
    strcpy(fullpath, path);
    strcat(fullpath, "/");
    strcat(fullpath, childpath);
    if(lstat(fullpath, &sb)) {
      send_errno_status(job);
      return;
    }
    stat_to_attrs(job->a, &sb, &d[n], 0xFFFFFFFF);
    d[n].name = childpath;
    ++n;
  }
  
  if(errno) {
    send_errno_status(job);
    return;
  }
  if(n) {
    send_begin(job);
    send_uint8(job, SSH_FXP_NAME);
    send_uint32(job, job->id);
    protocol->sendnames(job, n, d);
    send_end(job);
  } else
    send_status(job, SSH_FX_EOF, "end of directory list");
}

void sftp_close(struct sftpjob *job) {
  struct handleid id;
  
  pcheck(parse_handle(job, &id));
  D(("sftp_close %"PRIu32" %"PRIu32, id.id, id.tag));
  send_status(job, handle_close(&id), "closing handle");
}

void sftp_v345_realpath(struct sftpjob *job) {
  char *path, *resolved;
  struct sftpattr attr;

  pcheck(parse_path(job, &path));
  D(("sftp_realpath %s", path));
  memset(&attr, 0, sizeof attr);
  /* realpath() demands a buffer of PATH_MAX bytes.  PATH_MAX isn't actually
   * guaranteed to be a constant so we must allocate in dynamically.  We add a
   * bit of extra space as a guard against broken C libraries. */
  attr.name = resolved = alloc(job->a, PATH_MAX + 64);
  if(realpath(path, resolved)) {
    D(("...real path is %s", attr.name));
    send_begin(job);
    send_uint8(job, SSH_FXP_NAME);
    send_uint32(job, job->id);
    protocol->sendnames(job, 1, &attr);
    send_end(job);
  } else
    send_errno_status(job);
}

/* Command code for the various _*STAT calls.  rc is the return value
 * from *stat() and SB is the buffer. */
static void sftp_v3_stat_core(struct sftpjob *job, int rc, 
                              const struct stat *sb) {
  struct sftpattr attrs;

  if(!rc) {
    /* We suppress owner/group name lookup since there is no way to communicate
     * it in protocol version 3 */
    stat_to_attrs(job->a, sb, &attrs, ~(uint32_t)SSH_FILEXFER_ATTR_OWNERGROUP);
    send_begin(job);
    send_uint8(job, SSH_FXP_ATTRS);
    send_uint32(job, job->id);
    protocol->sendattrs(job, &attrs);
    send_end(job);
  } else
    send_errno_status(job);
}

static void sftp_v3_lstat(struct sftpjob *job) {
  char *path;
  struct stat sb;

  pcheck(parse_path(job, &path));
  D(("sftp_lstat %s", path));
  sftp_v3_stat_core(job, lstat(path, &sb), &sb);
}

static void sftp_v3_stat(struct sftpjob *job) {
  char *path;
  struct stat sb;

  pcheck(parse_path(job, &path));
  D(("sftp_stat %s", path));
  sftp_v3_stat_core(job, stat(path, &sb), &sb);
}

static void sftp_v3_fstat(struct sftpjob *job) {
  int fd;
  struct handleid id;
  struct stat sb;
  uint32_t rc;

  pcheck(parse_handle(job, &id));
  D(("sftp_fstat %"PRIu32" %"PRIu32, id.id, id.tag));
  if((rc = handle_get_fd(&id, &fd, 0, 0))) {
    send_status(job, rc, "invalid file handle");
    return;
  }
  serialize_on_handle(job, 0);
  sftp_v3_stat_core(job, fstat(fd, &sb), &sb);
}

void sftp_setstat(struct sftpjob *job) {
  char *path;
  struct sftpattr attrs;

  pcheck(parse_path(job, &path));
  pcheck(protocol->parseattrs(job, &attrs));
  D(("sftp_setstat %s", path));
  if(set_status(path, &attrs))
    send_errno_status(job);
  else
    send_ok(job);
}

void sftp_fsetstat(struct sftpjob *job) {
  struct handleid id;
  struct sftpattr attrs;
  int fd;
  uint32_t rc;

  pcheck(parse_handle(job, &id));
  pcheck(protocol->parseattrs(job, &attrs));
  D(("sftp_fsetstat %"PRIu32" %"PRIu32, id.id, id.tag));
  if((rc = handle_get_fd(&id, &fd, 0, 0))) {
    send_status(job, rc, "invalid file handle");
    return;
  }
  serialize_on_handle(job, 0);
  if(set_fstatus(fd, &attrs))
    send_errno_status(job);
  else
    send_ok(job);
}

void sftp_mkdir(struct sftpjob *job) {
  char *path;
  struct sftpattr attrs;

  pcheck(parse_path(job, &path));
  pcheck(protocol->parseattrs(job, &attrs));
  D(("sftp_mkdir %s", path));
  attrs.valid &= ~SSH_FILEXFER_ATTR_SIZE; /* makes no sense */
  if(attrs.valid & SSH_FILEXFER_ATTR_PERMISSIONS) {
    /* If we're given initial permissions, use them and don't reset them  */
    if(mkdir(path, attrs.permissions & 0777) < 0) {
      send_errno_status(job);
      return;
    }
    attrs.valid &= ~SSH_FILEXFER_ATTR_PERMISSIONS;
  } else {
    /* Otherwise follow the current umask */
    if(mkdir(path, 0777) < 0) {
      send_errno_status(job);
      return;
    }
  }
  if(set_status(path, &attrs)) {
    send_errno_status(job);
    /* If we can't have the desired permissions, don't have the directory at
     * all */
    rmdir(path);
    return;
  }
  send_ok(job);
}

void sftp_v34_open(struct sftpjob *job) {
  char *path;
  uint32_t pflags;
  struct sftpattr attrs;
  int open_flags, fd;
  struct handleid id;

  pcheck(parse_path(job, &path));
  pcheck(parse_uint32(job, &pflags));
  pcheck(protocol->parseattrs(job, &attrs));
  D(("sftp_open %s %#"PRIx32, path, pflags));
  /* Translate pflags to open(2) format */
  switch(pflags & (SSH_FXF_READ|SSH_FXF_WRITE)) {
  case SSH_FXF_READ:
    open_flags = O_RDONLY;
    break;
  case SSH_FXF_WRITE:
    open_flags = O_WRONLY;
    break;
  case SSH_FXF_READ|SSH_FXF_WRITE:
    open_flags = O_RDWR;
    break;
  default:
    send_status(job, SSH_FX_BAD_MESSAGE, "need SSH_FXF_READ or SSH_FXF_WRITE");
    return;
  }
  /* Append only makes sense for writable files */
  if(pflags & SSH_FXF_WRITE)
    if(pflags & SSH_FXF_APPEND)
      open_flags |= O_APPEND;
  if(pflags & SSH_FXF_CREAT) {
    open_flags |= O_CREAT;
    /* Truncate and no-overwrite only make sense if creating */
    if(pflags & SSH_FXF_TRUNC) open_flags |= O_TRUNC;
    if(pflags & SSH_FXF_EXCL) open_flags |= O_EXCL;
  }
  if(attrs.valid & SSH_FILEXFER_ATTR_PERMISSIONS) {
    /* If we're given initial permissions, use them and don't reset them  */
    if((fd = open(path, open_flags, attrs.permissions & 0777)) < 0) {
      send_errno_status(job);
      return;
    }
    attrs.valid &= ~SSH_FILEXFER_ATTR_PERMISSIONS;
  } else {
    /* Otherwise follow the current umask */
    if((fd = open(path, open_flags, 0777)) < 0) {
      send_errno_status(job);
      return;
    }
  }
  if(set_fstatus(fd, &attrs)) { 
    send_errno_status(job);
    close(fd);
    unlink(path);
    return;                             /* already sent error */
  }
  handle_new_file(&id, fd, path, !!(pflags & SSH_FXF_TEXT));
  D(("...handle is %"PRIu32" %"PRIu32, id.id, id.tag));
  send_begin(job);
  send_uint8(job, SSH_FXP_HANDLE);
  send_uint32(job, job->id);
  send_handle(job, &id);
  send_end(job);
}

void sftp_read(struct sftpjob *job) {
  struct handleid id;
  uint64_t offset;
  uint32_t len, rc;
  ssize_t n;
  int fd, istext;

  pcheck(parse_handle(job, &id));
  pcheck(parse_uint64(job, &offset));
  pcheck(parse_uint32(job, &len));
  D(("sftp_read %"PRIu32" %"PRIu32": %"PRIu32" bytes at %"PRIu64,
     id.id, id.tag, len, offset));
  if(len > MAXREAD) len = MAXREAD;
  if((rc = handle_get_fd(&id, &fd, 0,  &istext))) {
    send_status(job, rc, "invalid file handle");
    return;
  }
  serialize_on_handle(job, istext);
  /* We read straight into our own output buffer to save a copy. */
  send_begin(job);
  send_uint8(job, SSH_FXP_DATA);
  send_uint32(job, job->id);
  send_need(job->worker, len + 4);
  if(istext)
    n = read(fd, job->worker->buffer + job->worker->bufused + 4, len);
  else
    n = pread(fd, job->worker->buffer + job->worker->bufused + 4, len, offset);
  /* Short reads are allowed so we don't try to read more */
  if(n > 0) {
    /* Fix up the buffer */
    send_uint32(job, n);
    job->worker->bufused += n;
    send_end(job);
    return;
  }
  /* The error-sending code calls send_begin(), so we don't get half a
   * SSH_FXP_DATA response first */
  if(n == 0)
    send_status(job, SSH_FX_EOF, 0);
  else
    send_errno_status(job);
}

void sftp_write(struct sftpjob *job) {
  struct handleid id;
  uint64_t offset;
  uint32_t len, rc;
  ssize_t n;
  int fd, istext;

  pcheck(parse_handle(job, &id));
  pcheck(parse_uint64(job, &offset));
  pcheck(parse_uint32(job, &len));
  if(len > job->left) {
    send_status(job, SSH_FX_BAD_MESSAGE, "truncated SSH_FXP_WRITE");
    return;
  }
  D(("sftp_write %"PRIu32" %"PRIu32": %"PRIu32" bytes at %"PRIu64,
     id.id, id.tag, len, offset));
  if((rc = handle_get_fd(&id, &fd, 0, &istext))) {
    send_status(job, rc, "invalid file handle");
    return;
  }
  serialize_on_handle(job, istext);
  while(len > 0) {
    /* Short writes aren't allowed so we loop around writing more */
    if(istext)
      n = write(fd, job->ptr, len);
    else
      n = pwrite(fd, job->ptr, len, offset);
    if(n < 0) {
      send_errno_status(job);
      return;
    }
    job->ptr += n;
    job->left += n;
    len -= n;
    offset += n;
  }
  send_ok(job);
}

static const struct sftpcmd sftpv3tab[] = {
  { SSH_FXP_INIT, sftp_already_init },
  { SSH_FXP_OPEN, sftp_v34_open },
  { SSH_FXP_CLOSE, sftp_close },
  { SSH_FXP_READ, sftp_read },
  { SSH_FXP_WRITE, sftp_write },
  { SSH_FXP_LSTAT, sftp_v3_lstat },
  { SSH_FXP_FSTAT, sftp_v3_fstat },
  { SSH_FXP_SETSTAT, sftp_setstat },
  { SSH_FXP_FSETSTAT, sftp_fsetstat },
  { SSH_FXP_OPENDIR, sftp_opendir },
  { SSH_FXP_READDIR, sftp_readdir },
  { SSH_FXP_REMOVE, sftp_remove },
  { SSH_FXP_MKDIR, sftp_mkdir },
  { SSH_FXP_RMDIR, sftp_rmdir },
  { SSH_FXP_REALPATH, sftp_v345_realpath },
  { SSH_FXP_STAT, sftp_v3_stat },
  { SSH_FXP_RENAME, sftp_v34_rename },
  { SSH_FXP_READLINK, sftp_readlink },
  { SSH_FXP_SYMLINK, sftp_symlink },
  //{ SSH_FXP_EXTENDED, sftp_extended },
};

const struct sftpprotocol sftpv3 = {
  sizeof sftpv3tab / sizeof (struct sftpcmd), /* ncommands */
  sftpv3tab,                                  /* commands */
  3,                                          /* version */
  0xFFFFFFFF,                                 /* attrbits */
  SSH_FX_OP_UNSUPPORTED,                      /* maxstatus */
  v3_sendnames,
  v3_sendattrs,
  v3_parseattrs,
  v3_encode,
  v3_decode
};

/*
Local Variables:
c-basic-offset:2
comment-column:40
fill-column:79
indent-tabs-mode:nil
End:
*/
