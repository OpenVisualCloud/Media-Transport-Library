/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2023 Intel Corporation
 */

/*
 * Socket path rules, shared by MtlManager and libmtlm_client so that the two
 * can never disagree about where the socket is. See mtlm_api.h for the order.
 */

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "mtlm_api.h"

/* Copy src into buf, refusing a path an AF_UNIX address cannot hold. */
static int path_set(char* buf, size_t len, const char* src) {
  size_t src_len = strlen(src);

  if (src_len == 0) return -EINVAL;
  if (src_len >= MTLM_SOCK_PATH_MAX) return -ENAMETOOLONG;
  if (src_len >= len) return -ENAMETOOLONG;

  memcpy(buf, src, src_len + 1);
  return 0;
}

static int path_format(char* buf, size_t len, const char* fmt, ...) {
  char tmp[MTLM_SOCK_PATH_MAX * 2];
  va_list ap;
  int ret;

  va_start(ap, fmt);
  ret = vsnprintf(tmp, sizeof(tmp), fmt, ap);
  va_end(ap);

  if (ret < 0) return -EINVAL;
  if ((size_t)ret >= sizeof(tmp)) return -ENAMETOOLONG;

  return path_set(buf, len, tmp);
}

/* The path set in the environment, or NULL when it is unset or empty. */
static const char* env_path(void) {
  const char* env = getenv(MTL_MANAGER_SOCK_ENV);

  if (env == NULL || env[0] == '\0') return NULL;
  return env;
}

/* Per-user path: below XDG_RUNTIME_DIR when the session provides one. */
static int user_path(char* buf, size_t len) {
  const char* runtime_dir = getenv("XDG_RUNTIME_DIR");

  if (runtime_dir != NULL && runtime_dir[0] == '/')
    return path_format(buf, len, "%s/imtl/%s", runtime_dir, MTL_MANAGER_SOCK_NAME);

  return path_format(buf, len, "/tmp/imtl-%u/%s", (unsigned int)getuid(),
                     MTL_MANAGER_SOCK_NAME);
}

int mtlm_sock_path_candidate(unsigned int index, char* buf, size_t len) {
  const char* env = env_path();

  if (buf == NULL || len == 0) return -EINVAL;
  buf[0] = '\0';

  /* An explicit path is the only path. Silently falling back to another one
   * would send a client to a manager the operator did not ask for. */
  if (env != NULL) {
    if (index != 0) return -ENOENT;
    return path_set(buf, len, env);
  }

  if (geteuid() == 0) {
    if (index == 0) return path_set(buf, len, MTL_MANAGER_SOCK_PATH);
    if (index == 1) return user_path(buf, len);
    return -ENOENT;
  }

  if (index == 0) return user_path(buf, len);
  if (index == 1) return path_set(buf, len, MTL_MANAGER_SOCK_PATH);
  return -ENOENT;
}

int mtlm_sock_path_resolve(char* buf, size_t len) {
  return mtlm_sock_path_candidate(0, buf, len);
}

const char* mtlm_sock_path(void) {
  static char path[MTLM_SOCK_PATH_MAX];

  if (mtlm_sock_path_resolve(path, sizeof(path)) < 0) return NULL;
  return path;
}

/* mkdir every missing component of dir. */
static int mkdir_parents(const char* dir, mode_t mode) {
  char tmp[MTLM_SOCK_PATH_MAX];
  size_t len = strlen(dir);
  char* p;

  if (len == 0) return -EINVAL;
  if (len >= sizeof(tmp)) return -ENAMETOOLONG;
  memcpy(tmp, dir, len + 1);

  /* Skip the leading slash so an absolute path does not try to mkdir "". */
  for (p = tmp + 1; *p != '\0'; p++) {
    if (*p != '/') continue;
    *p = '\0';
    if (mkdir(tmp, mode) < 0 && errno != EEXIST) return -errno;
    *p = '/';
  }

  if (mkdir(tmp, mode) < 0 && errno != EEXIST) return -errno;
  return 0;
}

int mtlm_sock_dir_prepare(const char* path, unsigned int mode) {
  char dir[MTLM_SOCK_PATH_MAX];
  const char* slash;
  size_t dir_len;

  if (path == NULL) return -EINVAL;

  slash = strrchr(path, '/');
  if (slash == NULL) return -EINVAL; /* a relative name has no directory */
  if (slash == path) return 0;       /* the socket sits in the root directory */

  dir_len = (size_t)(slash - path);
  if (dir_len >= sizeof(dir)) return -ENAMETOOLONG;
  memcpy(dir, path, dir_len);
  dir[dir_len] = '\0';

  return mkdir_parents(dir, (mode_t)mode);
}

bool mtlm_sock_path_is_system(const char* path) {
  if (path == NULL) return false;
  return strcmp(path, MTL_MANAGER_SOCK_PATH) == 0;
}
