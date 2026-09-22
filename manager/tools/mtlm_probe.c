/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2023 Intel Corporation
 */

/*
 * MtlManagerProbe: a conformance and abuse test of a running MtlManager.
 *
 * It uses the public API only. The two includes below are the installed
 * headers, and the one library it links is libmtlm_client. It knows nothing
 * about the source of the daemon, so it tests the contract and not the
 * implementation. Every operation the manager can perform has a case here.
 *
 *   MtlManagerProbe                      every group that needs no interface
 *   MtlManagerProbe --ifname eth0 all    every group
 *   MtlManagerProbe --list               name each group, run nothing
 *   MtlManagerProbe abuse                malformed input only
 *
 * The exit status is 0 when no case failed, 1 when one did, and 2 for a usage
 * or environment problem. A case that cannot run on this host or with these
 * rights reports SKIP with the reason, and SKIP is not a failure.
 *
 * WARNING about --ifname. The manager deletes every receive flow rule of an
 * interface when it first takes it, so naming an interface that carries live
 * traffic removes the rules of that traffic. Use an interface of your own. A
 * veth pair costs nothing:
 *
 *   sudo ip link add probe0 type veth peer name probe1
 *   sudo ip link set probe0 up && sudo ip link set probe1 up
 *   sudo MtlManagerProbe --ifname probe0 all
 *
 * The XDP groups need root, because attaching a program to an interface does.
 * Without root they report SKIP and say so.
 */

/* The two public headers, and nothing else of MTL. `pkg-config --cflags
 * mtlm_client` gives both ${includedir} and ${includedir}/mtl, so this spelling
 * and <mtl/mtlm_api.h> both work for a program outside this tree. This one is
 * used because it also works in tree, where the headers are in ../../include
 * and no directory is named mtl. */
#include <arpa/inet.h>
#include <errno.h>
#include <linux/if_link.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <mtl_mproto.h>
#include <mtlm_api.h>
#include <net/if.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

/* Largest number of clients one case opens at once. Kept below the default
 * --max-clients of the manager, so a refusal in this test means a fault and not
 * the limit doing its job. */
#define PROBE_MANY_CLIENTS (16)

/* Records one case sends in a single write. 50 of them pass 4 KiB, which is the
 * read chunk of the manager, so the last record arrives in two pieces. That is
 * the case that proves the manager keeps a partial tail. */
#define PROBE_BURST_RECORDS (50)

/* How long a case waits for a record the manager should already have sent. */
#define PROBE_REPLY_TIMEOUT_MS (2000)

/* How long a case waits for a resource of a dead client to come back. The
 * manager frees it when it notices the closed socket, which is one epoll turn,
 * but a loaded host can take longer. */
#define PROBE_RECLAIM_TIMEOUT_MS (5000)

static unsigned int g_pass;
static unsigned int g_fail;
static unsigned int g_skip;
static bool g_verbose;
static const char* g_sock_path; /* NULL means the candidate search order */
static const char* g_ifname;
static unsigned int g_ifindex;
static const char* g_group; /* the group that is running, for the report */

/*
 * Reporting
 */

static void report(const char* verdict, const char* name, const char* fmt, va_list ap) {
  printf("[%-4s] %s.%s", verdict, g_group, name);
  if (fmt != NULL && fmt[0] != '\0') {
    printf(" -- ");
    vprintf(fmt, ap);
  }
  printf("\n");
  fflush(stdout);
}

static void pass(const char* name, const char* fmt, ...) {
  va_list ap;

  g_pass++;
  if (!g_verbose) return;

  va_start(ap, fmt);
  report("PASS", name, fmt, ap);
  va_end(ap);
}

static void fail(const char* name, const char* fmt, ...) {
  va_list ap;

  g_fail++;
  va_start(ap, fmt);
  report("FAIL", name, fmt, ap);
  va_end(ap);
}

static void skip(const char* name, const char* fmt, ...) {
  va_list ap;

  g_skip++;
  va_start(ap, fmt);
  report("SKIP", name, fmt, ap);
  va_end(ap);
}

/** Text of a return value, which is an errno for a negative one. */
static const char* ret_text(int ret) {
  return mtlm_strerror(ret);
}

/** Check that `got` is `want`. @return true when it is. */
static bool check_eq(const char* name, int got, int want) {
  if (got == want) {
    pass(name, "%d (%s)", got, ret_text(got));
    return true;
  }

  fail(name, "want %d (%s), got %d (%s)", want, ret_text(want), got, ret_text(got));
  return false;
}

/** Check that `got` is `want`, for a value that is not a return code. */
static bool check_num(const char* name, long got, long want) {
  if (got == want) {
    pass(name, "%ld", got);
    return true;
  }

  fail(name, "want %ld, got %ld", want, got);
  return false;
}

/** Check that `got` is `low` or more. */
static bool check_ge(const char* name, int got, int low) {
  if (got >= low) {
    pass(name, "%d", got);
    return true;
  }

  fail(name, "want %d or more, got %d (%s)", low, got, ret_text(got));
  return false;
}

static bool check_true(const char* name, bool cond, const char* fmt, ...) {
  va_list ap;

  if (cond) {
    pass(name, "%s", "");
    return true;
  }

  va_start(ap, fmt);
  report("FAIL", name, fmt, ap);
  va_end(ap);
  g_fail++;
  return false;
}

static bool check_str(const char* name, const char* got, const char* want) {
  if (got != NULL && want != NULL && strcmp(got, want) == 0) {
    pass(name, "%s", got);
    return true;
  }

  fail(name, "want \"%s\", got \"%s\"", want == NULL ? "(null)" : want,
       got == NULL ? "(null)" : got);
  return false;
}

/*
 * Small helpers
 */

static void sleep_ms(unsigned int ms) {
  struct timespec ts;

  ts.tv_sec = ms / 1000;
  ts.tv_nsec = (long)(ms % 1000) * 1000000L;
  nanosleep(&ts, NULL);
}

/**
 * A connected and registered client. NULL on failure.
 *
 * With --ifname it names that interface, the same as an MTL instance names its
 * AF_XDP ports. That is what makes the manager load the XDP program, so the
 * filter and xdp groups have something to work with. The manager needs the
 * program for an interface named this way, so a host that cannot attach one
 * answers -ENODEV; this then registers with no interface and records why, so
 * the groups that need XDP report SKIP with the reason instead of FAIL.
 */
static bool g_xdp_attached;
static int g_xdp_attach_ret;

static mtlm_client* new_registered(void) {
  struct mtlm_register_args args;
  mtlm_client* client = mtlm_client_create(g_sock_path);
  int ret;

  if (client == NULL) return NULL;

  memset(&args, 0, sizeof(args));
  args.uid = -1; /* the API fills in the real one */

  if (g_ifindex != 0) {
    args.ifindex = &g_ifindex;
    args.num_if = 1;

    ret = mtlm_register(client, &args);
    if (ret == 0) {
      g_xdp_attached = true;
      return client;
    }

    g_xdp_attach_ret = ret;
    args.ifindex = NULL;
    args.num_if = 0;
  }

  if (mtlm_register(client, &args) < 0) {
    mtlm_client_destroy(client);
    return NULL;
  }

  return client;
}

/**
 * Whether a manager still answers and still serves.
 *
 * Every abuse case calls this afterwards. A manager that survived the record
 * but stopped serving would otherwise look like a pass.
 */
static bool manager_serving(void) {
  mtlm_client* client = mtlm_client_create(g_sock_path);
  uint32_t acked = 0;
  bool ok;

  if (client == NULL) return false;

  ok = mtlm_heartbeat(client, 0x5a5a5a5au, &acked) == 0 && acked == 0x5a5a5a5au;
  if (ok) {
    struct mtlm_register_args args;

    memset(&args, 0, sizeof(args));
    args.uid = -1;
    ok = mtlm_register(client, &args) == 0;
  }

  mtlm_client_destroy(client);
  return ok;
}

/** An lcore id no one holds, or -1 when every id is taken. */
static int find_free_lcore(mtlm_client* client) {
  int id;

  /* From the top down. A real MTL instance takes the low ids first, so this
   * keeps the probe out of its way. */
  for (id = MTL_MANAGER_MAX_LCORE - 1; id >= 0; id--)
    if (mtlm_lcore_get(client, (uint16_t)id) == 0) return id;

  return -1;
}

/*
 * Wire helpers, for the abuse group only.
 *
 * mtl_mproto.h is public for this reason: a test must be able to put a record
 * on the socket that the client library would never build.
 */

static void wire_init(mtl_message_t* msg, uint32_t type, uint32_t body_len) {
  memset(msg, 0, sizeof(*msg));
  msg->header.magic = htonl(MTL_MANAGER_MAGIC);
  msg->header.type = (mtl_message_type_t)htonl(type);
  msg->header.body_len = htonl(body_len);
}

/** Write every byte, or return a negative errno. */
static int wire_send(int fd, const void* buf, size_t len) {
  const char* at = buf;
  size_t done = 0;

  while (done < len) {
    ssize_t ret = send(fd, at + done, len - done, MSG_NOSIGNAL);

    if (ret > 0) {
      done += (size_t)ret;
      continue;
    }
    if (ret < 0 && errno == EINTR) continue;
    return ret < 0 ? -errno : -EPIPE;
  }

  return 0;
}

/**
 * Read one whole record.
 *
 * @return 0 on success, -ETIMEDOUT when nothing arrives in time, -ECONNRESET
 *         when the manager closed the connection.
 */
static int wire_read(int fd, mtl_message_t* msg, int timeout_ms) {
  char* at = (char*)msg;
  size_t done = 0;

  while (done < MTL_MANAGER_MSG_SIZE) {
    struct pollfd pfd;
    ssize_t ret;

    pfd.fd = fd;
    pfd.events = POLLIN;
    pfd.revents = 0;
    ret = poll(&pfd, 1, timeout_ms);
    if (ret == 0) return -ETIMEDOUT;
    if (ret < 0) {
      if (errno == EINTR) continue;
      return -errno;
    }

    ret = recv(fd, at + done, MTL_MANAGER_MSG_SIZE - done, 0);
    if (ret == 0) return -ECONNRESET;
    if (ret < 0) {
      if (errno == EINTR) continue;
      return -errno;
    }
    done += (size_t)ret;
  }

  return 0;
}

/** The response field of a record, in host order. */
static int wire_response(const mtl_message_t* msg) {
  return (int)ntohl((uint32_t)msg->body.response_msg.response);
}

/*
 * Group: path. The socket path rules. It needs no manager.
 */

static void group_path(void) {
  char buf[MTLM_SOCK_PATH_MAX];
  char first[MTLM_SOCK_PATH_MAX];
  char second[MTLM_SOCK_PATH_MAX];
  char saved[MTLM_SOCK_PATH_MAX];
  const char* from_env;
  const char* resolved;
  char dir_path[512];
  struct stat st;
  bool had_env;

  g_group = "path";

  resolved = mtlm_sock_path();
  check_true("mtlm_sock_path gives a path", resolved != NULL, "got NULL");

  if (resolved != NULL) {
    check_eq("mtlm_sock_path_resolve agrees with mtlm_sock_path",
             mtlm_sock_path_resolve(buf, sizeof(buf)), 0);
    check_str("the two spellings are the same path", buf, resolved);
  }

  check_eq("a NULL buffer is refused", mtlm_sock_path_resolve(NULL, sizeof(buf)),
           -EINVAL);
  check_eq("a zero length is refused", mtlm_sock_path_resolve(buf, 0), -EINVAL);
  check_eq("a buffer too small is refused", mtlm_sock_path_resolve(buf, 2),
           -ENAMETOOLONG);

  /* The environment variable wins over everything, and it leaves one candidate
   * only, so a client cannot reach a manager the operator did not name. */
  from_env = getenv(MTL_MANAGER_SOCK_ENV);
  had_env = from_env != NULL;
  if (had_env) snprintf(saved, sizeof(saved), "%s", from_env);

  setenv(MTL_MANAGER_SOCK_ENV, "/tmp/mtlm-probe-from-the-environment.sock", 1);
  check_eq("with the variable set the path resolves",
           mtlm_sock_path_resolve(buf, sizeof(buf)), 0);
  check_str("the variable wins", buf, "/tmp/mtlm-probe-from-the-environment.sock");
  check_eq("and it is the only candidate", mtlm_sock_path_candidate(1, buf, sizeof(buf)),
           -ENOENT);

  unsetenv(MTL_MANAGER_SOCK_ENV);
  if (check_eq("without the variable there is a first candidate",
               mtlm_sock_path_candidate(0, first, sizeof(first)), 0) &&
      check_eq("and a second one", mtlm_sock_path_candidate(1, second, sizeof(second)),
               0)) {
    check_true("the two candidates differ", strcmp(first, second) != 0, "both are \"%s\"",
               first);
    /* A program without root must look at its own runtime directory first, or
     * it would need the system directory to exist at all. */
    if (geteuid() != 0) {
      check_true("a non-root client looks at its own path first",
                 !mtlm_sock_path_is_system(first), "\"%s\" is the system path", first);
      check_true("and at the system path second", mtlm_sock_path_is_system(second),
                 "\"%s\" is not the system path", second);
    } else {
      check_true("root looks at the system path first", mtlm_sock_path_is_system(first),
                 "\"%s\" is not the system path", first);
    }
  }
  check_eq("there is no third candidate",
           mtlm_sock_path_candidate(MTLM_SOCK_PATH_CANDIDATES, buf, sizeof(buf)),
           -ENOENT);

  if (had_env) setenv(MTL_MANAGER_SOCK_ENV, saved, 1);

  check_true("the system path is recognised",
             mtlm_sock_path_is_system(MTL_MANAGER_SOCK_PATH), "it is not");
  check_true("a path below /tmp is not the system path",
             !mtlm_sock_path_is_system("/tmp/whatever.sock"), "it is");
  check_true("a NULL path is not the system path", !mtlm_sock_path_is_system(NULL),
             "it is");

  /* The directory helper. A manager calls it before it binds. */
  snprintf(dir_path, sizeof(dir_path), "/tmp/mtlm-probe-%d/a/b/c/mtl_manager.sock",
           (int)getpid());
  check_eq("a missing directory is created", mtlm_sock_dir_prepare(dir_path, 0700), 0);
  snprintf(buf, sizeof(buf), "/tmp/mtlm-probe-%d/a/b/c", (int)getpid());
  if (check_true("the directory is there", stat(buf, &st) == 0, "%s", strerror(errno)))
    check_num("and it has the mode it was given", (long)(st.st_mode & 07777), 0700);
  check_eq("a second call changes nothing", mtlm_sock_dir_prepare(dir_path, 0700), 0);
  check_eq("a NULL path is refused", mtlm_sock_dir_prepare(NULL, 0700), -EINVAL);
  check_eq("a name with no directory is refused", mtlm_sock_dir_prepare("sock", 0700),
           -EINVAL);

  /* A directory part that cannot fit an AF_UNIX address must be refused here,
   * and not by bind() much later. */
  memset(dir_path, 'x', sizeof(dir_path) - 1);
  dir_path[0] = '/';
  dir_path[sizeof(dir_path) - 1] = '\0';
  dir_path[MTLM_SOCK_PATH_MAX + 4] = '/';
  check_eq("a directory that cannot fit an address is refused",
           mtlm_sock_dir_prepare(dir_path, 0700), -ENAMETOOLONG);

  /* A parent that is a file, not a directory. The errno of mkdir must come
   * back, so the manager can say why it could not bind. */
  snprintf(dir_path, sizeof(dir_path), "/tmp/mtlm-probe-file-%d", (int)getpid());
  {
    FILE* f = fopen(dir_path, "w");

    if (f == NULL) {
      skip("could not make a file to test a bad parent", "%s", strerror(errno));
    } else {
      char under[600];

      fclose(f);
      snprintf(under, sizeof(under), "%s/below/mtl_manager.sock", dir_path);
      check_eq("a parent that is a file is refused", mtlm_sock_dir_prepare(under, 0700),
               -ENOTDIR);
      unlink(dir_path);
    }
  }

  /* Clean up, deepest first. */
  snprintf(buf, sizeof(buf), "/tmp/mtlm-probe-%d/a/b/c", (int)getpid());
  rmdir(buf);
  snprintf(buf, sizeof(buf), "/tmp/mtlm-probe-%d/a/b", (int)getpid());
  rmdir(buf);
  snprintf(buf, sizeof(buf), "/tmp/mtlm-probe-%d/a", (int)getpid());
  rmdir(buf);
  snprintf(buf, sizeof(buf), "/tmp/mtlm-probe-%d", (int)getpid());
  rmdir(buf);
}

/*
 * Group: api. The shape of the API. It needs no manager either.
 */

static void group_api(void) {
  static const int values[] = {0,       -EINVAL,  -EPERM, -EBUSY,      -ENODEV,
                               -ENOSPC, -ENOTSUP, -EPIPE, -ECONNRESET, 7};
  unsigned int i;
  const char* version;
  unsigned int major = 0;
  unsigned int minor = 0;

  g_group = "api";

  version = mtlm_proto_version();
  if (check_true("mtlm_proto_version gives text", version != NULL, "got NULL")) {
    check_num("it is two numbers", sscanf(version, "%u.%u", &major, &minor), 2);
    check_num("the major matches the header", (long)major,
              (long)MTL_MANAGER_PROTO_VERSION_MAJOR);
    check_num("the minor matches the header", (long)minor,
              (long)MTL_MANAGER_PROTO_VERSION_MINOR);
  }

  for (i = 0; i < sizeof(values) / sizeof(values[0]); i++) {
    char name[64];

    snprintf(name, sizeof(name), "mtlm_strerror has text for %d", values[i]);
    check_true(name, mtlm_strerror(values[i]) != NULL, "got NULL");
  }

  /* The record layout. A client of another language reads these numbers out of
   * the header, so a change of them is a change of the protocol. */
  check_num("one record is the size of mtl_message_t", (long)MTL_MANAGER_MSG_SIZE,
            (long)sizeof(mtl_message_t));
  check_num("the header is three 32 bit fields", (long)sizeof(mtl_message_header_t), 12);
  check_num("the record has no padding", (long)sizeof(mtl_message_t),
            (long)(sizeof(mtl_message_header_t) + sizeof(mtl_register_message_t)));
  check_num(
      "the register body is the largest body",
      (long)(sizeof(mtl_register_message_t) >= sizeof(mtl_if_message_t) &&
             sizeof(mtl_register_message_t) >= sizeof(mtl_lcore_message_t) &&
             sizeof(mtl_register_message_t) >= sizeof(mtl_heartbeat_message_t) &&
             sizeof(mtl_register_message_t) >= sizeof(mtl_udp_dp_filter_message_t) &&
             sizeof(mtl_register_message_t) >= sizeof(mtl_response_message_t)),
      1);

  /* Every call must survive a NULL client. A caller that did not test the
   * result of mtlm_client_create() has a fault, and it must be able to report
   * that fault rather than take the process down first. */
  mtlm_client_destroy(NULL);
  pass("mtlm_client_destroy accepts NULL", "%s", "");
  check_eq("mtlm_client_fd of NULL", mtlm_client_fd(NULL), -1);
  check_true("mtlm_client_sock_path of NULL", mtlm_client_sock_path(NULL) == NULL,
             "got text");
  check_eq("mtlm_register of NULL", mtlm_register(NULL, NULL), -EINVAL);
  check_eq("mtlm_heartbeat of NULL", mtlm_heartbeat(NULL, 1, NULL), -EINVAL);
  check_eq("mtlm_lcore_get of NULL", mtlm_lcore_get(NULL, 1), -EINVAL);
  check_eq("mtlm_lcore_put of NULL", mtlm_lcore_put(NULL, 1), -EINVAL);
  check_eq("mtlm_queue_get of NULL", mtlm_queue_get(NULL, 1), -EINVAL);
  check_eq("mtlm_queue_put of NULL", mtlm_queue_put(NULL, 1, 1), -EINVAL);
  check_eq("mtlm_flow_add of NULL", mtlm_flow_add(NULL, NULL), -EINVAL);
  check_eq("mtlm_flow_del of NULL", mtlm_flow_del(NULL, 1, 1), -EINVAL);
  check_eq("mtlm_udp_dp_filter_add of NULL", mtlm_udp_dp_filter_add(NULL, 1, 1), -EINVAL);
  check_eq("mtlm_udp_dp_filter_del of NULL", mtlm_udp_dp_filter_del(NULL, 1, 1), -EINVAL);
  check_eq("mtlm_xsk_map_fd of NULL", mtlm_xsk_map_fd(NULL, 1), -EINVAL);

  /* A path with no manager is a normal answer, not a crash and not a hang. */
  check_true("no manager on an unused path is not alive",
             !mtlm_manager_alive("/tmp/mtlm-probe-there-is-nothing-here.sock"), "it is");
  check_true("and a connect to it gives NULL",
             mtlm_client_create("/tmp/mtlm-probe-there-is-nothing-here.sock") == NULL,
             "it gave a client");
}

/*
 * Group: connect. Connect, register, heartbeat, and the rule that every other
 * operation needs a registered instance.
 */

static void group_connect(void) {
  struct mtlm_register_args args;
  unsigned int too_many[MTL_MANAGER_MAX_IF + 1];
  unsigned int bad_if = 0x00ffffffu;
  mtlm_client* client;
  uint32_t acked;
  unsigned int i;

  g_group = "connect";

  if (!check_true("a manager answers", mtlm_manager_alive(g_sock_path),
                  "nothing answers on %s",
                  g_sock_path != NULL ? g_sock_path : "the default path"))
    return;

  client = mtlm_client_create(g_sock_path);
  if (!check_true("a client connects", client != NULL, "%s", strerror(errno))) return;

  check_ge("the client has a socket", mtlm_client_fd(client), 0);
  check_true("the client reports its path", mtlm_client_sock_path(client) != NULL,
             "got NULL");
  if (g_sock_path != NULL)
    check_str("and it is the path asked for", mtlm_client_sock_path(client), g_sock_path);

  /* A heartbeat needs no registration. It is what a client uses to find out
   * whether the manager is still there. */
  acked = 0;
  check_eq("a heartbeat before register is answered", mtlm_heartbeat(client, 1, &acked),
           0);
  check_num("the sequence number comes back", (long)acked, 1);
  acked = 0;
  check_eq("a heartbeat of zero", mtlm_heartbeat(client, 0, &acked), 0);
  check_num("zero comes back", (long)acked, 0);
  acked = 0;
  check_eq("a heartbeat of the largest number",
           mtlm_heartbeat(client, 0xffffffffu, &acked), 0);
  check_num("it comes back whole", (long)acked, (long)0xffffffffu);
  check_eq("a heartbeat that throws the answer away", mtlm_heartbeat(client, 2, NULL), 0);

  /* Everything else is refused until the instance registers. */
  check_eq("lcore_get needs a registration", mtlm_lcore_get(client, 1), -EPERM);
  check_eq("lcore_put needs a registration", mtlm_lcore_put(client, 1), -EPERM);
  check_eq("queue_get needs a registration", mtlm_queue_get(client, 1), -EPERM);
  check_eq("queue_put needs a registration", mtlm_queue_put(client, 1, 1), -EPERM);
  check_eq("flow_del needs a registration", mtlm_flow_del(client, 1, 1), -EPERM);
  check_eq("filter_add needs a registration", mtlm_udp_dp_filter_add(client, 1, 1),
           -EPERM);
  check_eq("filter_del needs a registration", mtlm_udp_dp_filter_del(client, 1, 1),
           -EPERM);
  {
    struct mtlm_flow flow;

    memset(&flow, 0, sizeof(flow));
    flow.ifindex = 1;
    check_eq("flow_add needs a registration", mtlm_flow_add(client, &flow), -EPERM);
  }

  check_eq("a NULL argument list is refused", mtlm_register(client, NULL), -EINVAL);

  /* More interfaces than one record can hold. The API must refuse it before it
   * writes, because the manager would read past the end of the array. */
  for (i = 0; i < MTL_MANAGER_MAX_IF + 1; i++) too_many[i] = 1;
  memset(&args, 0, sizeof(args));
  args.uid = -1;
  args.ifindex = too_many;
  args.num_if = MTL_MANAGER_MAX_IF + 1;
  check_eq("too many interfaces are refused", mtlm_register(client, &args), -EINVAL);
  check_eq("and the connection still works", mtlm_heartbeat(client, 3, NULL), 0);
  check_eq("and the instance is still not registered", mtlm_lcore_get(client, 1), -EPERM);

  /* An interface that does not exist. The manager must say so and keep the
   * client, because the client can still ask about another interface. */
  memset(&args, 0, sizeof(args));
  args.uid = -1;
  args.ifindex = &bad_if;
  args.num_if = 1;
  check_eq("an unknown interface is refused", mtlm_register(client, &args), -ENODEV);
  check_eq("the connection survives it", mtlm_heartbeat(client, 4, NULL), 0);
  check_eq("and the instance is still not registered", mtlm_queue_get(client, 1), -EPERM);

  /* The plain case. Every field left at the value that asks the API to fill it
   * in, so this also covers getpid, getuid and gethostname. */
  memset(&args, 0, sizeof(args));
  args.uid = -1;
  check_eq("register with no interface", mtlm_register(client, &args), 0);
  check_eq("an operation is allowed now", mtlm_lcore_put(client, 1), -EINVAL);

  /* Registering twice is what an instance does after it re-reads its
   * configuration. It must not be an error and must not lose anything. */
  check_eq("register again", mtlm_register(client, &args), 0);
  check_eq("and the instance is still registered", mtlm_heartbeat(client, 5, NULL), 0);

  mtlm_client_destroy(client);

  /* Many clients at once, because one instance per host is not the case the
   * manager exists for. */
  {
    mtlm_client* clients[PROBE_MANY_CLIENTS];
    unsigned int opened = 0;

    for (i = 0; i < PROBE_MANY_CLIENTS; i++) {
      clients[i] = new_registered();
      if (clients[i] == NULL) break;
      opened++;
    }
    check_num("many clients register at once", (long)opened, (long)PROBE_MANY_CLIENTS);
    for (i = 0; i < opened; i++) mtlm_client_destroy(clients[i]);
  }

  check_true("the manager still serves", manager_serving(), "it does not");
}

/*
 * Group: lcore. Claim, release, and the rule that one lcore goes to one
 * instance. It needs no interface.
 */

static void group_lcore(void) {
  mtlm_client* owner;
  mtlm_client* rival;
  int lcore;
  int held[8];
  unsigned int count = 0;
  unsigned int i;

  g_group = "lcore";

  owner = new_registered();
  if (!check_true("a registered client", owner != NULL, "could not register")) return;

  rival = new_registered();
  if (!check_true("a second registered client", rival != NULL, "could not register")) {
    mtlm_client_destroy(owner);
    return;
  }

  /* An id the accounting does not cover. */
  check_eq("an lcore id at the limit is refused",
           mtlm_lcore_get(owner, MTL_MANAGER_MAX_LCORE), -EINVAL);
  check_eq("and so is one above it", mtlm_lcore_get(owner, MTL_MANAGER_MAX_LCORE + 1),
           -EINVAL);
  check_eq("a put of an id at the limit is refused",
           mtlm_lcore_put(owner, MTL_MANAGER_MAX_LCORE), -EINVAL);

  lcore = find_free_lcore(owner);
  if (lcore < 0) {
    skip("every lcore is taken", "another instance holds all %d", MTL_MANAGER_MAX_LCORE);
    mtlm_client_destroy(rival);
    mtlm_client_destroy(owner);
    return;
  }
  pass("an lcore is claimed", "%d", lcore);

  check_eq("the same client cannot claim it twice",
           mtlm_lcore_get(owner, (uint16_t)lcore), -EBUSY);
  check_eq("another client cannot claim it", mtlm_lcore_get(rival, (uint16_t)lcore),
           -EBUSY);
  check_eq("another client cannot release it", mtlm_lcore_put(rival, (uint16_t)lcore),
           -EINVAL);
  check_eq("the holder releases it", mtlm_lcore_put(owner, (uint16_t)lcore), 0);
  check_eq("a second release is refused", mtlm_lcore_put(owner, (uint16_t)lcore),
           -EINVAL);
  check_eq("and now another client can claim it", mtlm_lcore_get(rival, (uint16_t)lcore),
           0);
  check_eq("which it then releases", mtlm_lcore_put(rival, (uint16_t)lcore), 0);

  /* A client that goes away must give back every lcore it held, or the host
   * loses a core for every instance that ever crashed. */
  for (i = 0; i < sizeof(held) / sizeof(held[0]); i++) {
    held[i] = find_free_lcore(owner);
    if (held[i] < 0) break;
    count++;
  }
  if (count == 0) {
    skip("no free lcore to test the release on death", "%s", "every id is taken");
  } else {
    int taken = 0;
    int waited;

    mtlm_client_destroy(owner);
    owner = NULL;

    /* The manager frees them when it notices the closed socket. */
    for (waited = 0; waited < PROBE_RECLAIM_TIMEOUT_MS; waited += 20) {
      taken = 0;
      for (i = 0; i < count; i++)
        if (mtlm_lcore_get(rival, (uint16_t)held[i]) == 0) taken++;
      if ((unsigned int)taken == count) break;
      for (i = 0; i < count; i++) mtlm_lcore_put(rival, (uint16_t)held[i]);
      sleep_ms(20);
    }
    check_num("a dead client gives every lcore back", (long)taken, (long)count);
    for (i = 0; i < count; i++) mtlm_lcore_put(rival, (uint16_t)held[i]);
  }

  mtlm_client_destroy(rival);
  if (owner != NULL) mtlm_client_destroy(owner);
}

/*
 * Group: queue. Reserve and release a receive queue.
 */

static void group_queue(void) {
  mtlm_client* owner;
  mtlm_client* rival;
  int first;
  int second;

  g_group = "queue";

  owner = new_registered();
  if (!check_true("a registered client", owner != NULL, "could not register")) return;

  check_eq("a queue on an unknown interface", mtlm_queue_get(owner, 0x00ffffffu),
           -ENODEV);

  if (g_ifindex == 0) {
    skip("no interface to test a queue on", "%s", "give --ifname");
    mtlm_client_destroy(owner);
    return;
  }

  first = mtlm_queue_get(owner, g_ifindex);
  if (first < 0) {
    /* A device with no combined channel can hand out no queue. That is a
     * property of the device, not a fault of the manager. */
    skip("the interface hands out no queue", "%s reports %s", g_ifname, ret_text(first));
    check_true("the manager still serves", manager_serving(), "it does not");
    mtlm_client_destroy(owner);
    return;
  }

  check_ge("a queue is reserved", first, 1);
  check_true("it is never queue 0", first != 0, "queue 0 belongs to the kernel");

  second = mtlm_queue_get(owner, g_ifindex);
  if (second >= 0) {
    check_true("a second reservation is a different queue", second != first,
               "both are %d", first);
    check_eq("and it is released", mtlm_queue_put(owner, g_ifindex, (uint16_t)second), 0);
  } else {
    skip("only one queue is free", "a second get reports %s", ret_text(second));
  }

  check_eq("queue 0 cannot be released", mtlm_queue_put(owner, g_ifindex, 0), -EINVAL);
  check_eq("a queue that does not exist cannot be released",
           mtlm_queue_put(owner, g_ifindex, 0xfffe), -EINVAL);

  rival = new_registered();
  if (rival != NULL) {
    check_eq("another client cannot release this queue",
             mtlm_queue_put(rival, g_ifindex, (uint16_t)first), -EINVAL);
    mtlm_client_destroy(rival);
  }

  check_eq("the holder releases it", mtlm_queue_put(owner, g_ifindex, (uint16_t)first),
           0);
  check_eq("a second release is refused",
           mtlm_queue_put(owner, g_ifindex, (uint16_t)first), -EINVAL);

  /* Death gives the queue back, the same as for an lcore. */
  first = mtlm_queue_get(owner, g_ifindex);
  if (first >= 0) {
    int waited;
    int again = -1;

    mtlm_client_destroy(owner);
    owner = NULL;

    for (waited = 0; waited < PROBE_RECLAIM_TIMEOUT_MS; waited += 20) {
      mtlm_client* fresh = new_registered();

      if (fresh != NULL) {
        again = mtlm_queue_get(fresh, g_ifindex);
        if (again == first) {
          mtlm_client_destroy(fresh);
          break;
        }
        if (again >= 0) mtlm_queue_put(fresh, g_ifindex, (uint16_t)again);
        mtlm_client_destroy(fresh);
      }
      sleep_ms(20);
    }
    check_num("a dead client gives its queue back", (long)again, (long)first);
  }

  if (owner != NULL) mtlm_client_destroy(owner);
}

/*
 * Group: flow. Insert and delete an ethtool receive flow rule.
 */

static void group_flow(void) {
  struct mtlm_flow flow;
  mtlm_client* owner;
  mtlm_client* rival;
  int flow_id;

  g_group = "flow";

  owner = new_registered();
  if (!check_true("a registered client", owner != NULL, "could not register")) return;

  check_eq("a NULL rule is refused", mtlm_flow_add(owner, NULL), -EINVAL);

  memset(&flow, 0, sizeof(flow));
  flow.ifindex = 0x00ffffffu;
  flow.flow_type = 0x02; /* UDP_V4_FLOW */
  flow.queue_id = 1;
  flow.dst_ip = inet_addr("239.168.0.1");
  flow.dst_port = 20000;
  check_eq("a rule on an unknown interface", mtlm_flow_add(owner, &flow), -ENODEV);

  check_eq("a delete on an unknown interface", mtlm_flow_del(owner, 0x00ffffffu, 1),
           -EINVAL);

  if (g_ifindex == 0) {
    skip("no interface to test a rule on", "%s", "give --ifname");
    mtlm_client_destroy(owner);
    return;
  }

  check_eq("a rule this client never added cannot be deleted",
           mtlm_flow_del(owner, g_ifindex, 12345), -EINVAL);

  flow.ifindex = g_ifindex;
  flow_id = mtlm_flow_add(owner, &flow);
  if (flow_id < 0) {
    /* A device with no rule table cannot hold a rule. Report which answer it
     * gave, because -EOPNOTSUPP and -ENOSPC mean different things to an
     * operator. */
    skip("the interface holds no flow rule", "%s reports %s", g_ifname,
         ret_text(flow_id));
    check_true("the manager still serves", manager_serving(), "it does not");
    mtlm_client_destroy(owner);
    return;
  }

  check_ge("a rule is inserted", flow_id, 1);

  rival = new_registered();
  if (rival != NULL) {
    check_eq("another client cannot delete this rule",
             mtlm_flow_del(rival, g_ifindex, (uint32_t)flow_id), -EINVAL);
    mtlm_client_destroy(rival);
  }

  check_eq("the owner deletes it", mtlm_flow_del(owner, g_ifindex, (uint32_t)flow_id), 0);
  check_eq("a second delete is refused",
           mtlm_flow_del(owner, g_ifindex, (uint32_t)flow_id), -EINVAL);

  /* Death takes the rule away, or the NIC keeps steering traffic to a queue no
   * one reads. */
  flow_id = mtlm_flow_add(owner, &flow);
  if (flow_id >= 0) {
    mtlm_client_destroy(owner);
    owner = NULL;
    sleep_ms(200);
    check_true("the manager still serves after the owner of a rule died",
               manager_serving(), "it does not");
  }

  if (owner != NULL) mtlm_client_destroy(owner);
}

/*
 * Group: filter. The UDP destination port filter of the XDP program.
 */

static void group_filter(void) {
  mtlm_client* client;
  int ret;

  g_group = "filter";

  client = new_registered();
  if (!check_true("a registered client", client != NULL, "could not register")) return;

  check_eq("a filter on an unknown interface",
           mtlm_udp_dp_filter_add(client, 0x00ffffffu, 20000), -ENODEV);

  if (g_ifindex == 0) {
    skip("no interface to test the filter on", "%s", "give --ifname");
    mtlm_client_destroy(client);
    return;
  }

  ret = mtlm_udp_dp_filter_add(client, g_ifindex, 20000);
  if (ret < 0) {
    /* The filter lives in a map of the XDP program, so without an attached
     * program there is nothing to write. */
    skip("the filter is not available", "%s reports %s, and the XDP attach reported %s",
         g_ifname, ret_text(ret), ret_text(g_xdp_attach_ret));
    check_true("the manager still serves", manager_serving(), "it does not");
    mtlm_client_destroy(client);
    return;
  }

  pass("a port is added to the filter", "%s", "");

  /* Several sessions can want the same port, so the manager counts the
   * references and only the last delete clears the map. */
  check_eq("the same port again", mtlm_udp_dp_filter_add(client, g_ifindex, 20000), 0);
  check_eq("one delete leaves it set", mtlm_udp_dp_filter_del(client, g_ifindex, 20000),
           0);
  check_eq("the second delete clears it",
           mtlm_udp_dp_filter_del(client, g_ifindex, 20000), 0);
  check_eq("a third delete is refused", mtlm_udp_dp_filter_del(client, g_ifindex, 20000),
           -EINVAL);

  /* The two ends of the port range. */
  check_eq("port 0", mtlm_udp_dp_filter_add(client, g_ifindex, 0), 0);
  check_eq("port 0 removed", mtlm_udp_dp_filter_del(client, g_ifindex, 0), 0);
  check_eq("port 65535", mtlm_udp_dp_filter_add(client, g_ifindex, 65535), 0);
  check_eq("port 65535 removed", mtlm_udp_dp_filter_del(client, g_ifindex, 65535), 0);

  check_eq("a port that was never added cannot be removed",
           mtlm_udp_dp_filter_del(client, g_ifindex, 4321), -EINVAL);

  /* A client that dies with ports set must not leave them set for ever. The
   * program itself goes away with the last user of the interface, so this case
   * checks that the manager survives and still serves. */
  check_eq("a port set and then abandoned",
           mtlm_udp_dp_filter_add(client, g_ifindex, 20001), 0);
  mtlm_client_destroy(client);
  sleep_ms(200);
  check_true("the manager still serves", manager_serving(), "it does not");
}

/*
 * Group: xdp. The xsks map descriptor, which travels over SCM_RIGHTS.
 */

/**
 * Id of the XDP program attached to `ifname`, read over netlink.
 *
 * 0 means no program. Negative means the question could not be asked. It goes
 * through netlink and not through libbpf because this program links the client
 * library alone: it must see the interface the way the rest of the host sees it,
 * and not the way the manager reports it.
 */
static long if_xdp_prog_id(const char* ifname) {
  struct {
    struct nlmsghdr hdr;
    struct ifinfomsg ifi;
    char attrs[64];
  } req;
  char reply[8192];
  struct nlmsghdr* nh;
  struct rtattr* rta;
  size_t name_len = strlen(ifname) + 1;
  ssize_t len;
  int fd;

  if (name_len > sizeof(req.attrs) - RTA_SPACE(0)) return -ENAMETOOLONG;

  fd = socket(AF_NETLINK, SOCK_RAW, NETLINK_ROUTE);
  if (fd < 0) return -errno;

  memset(&req, 0, sizeof(req));
  req.hdr.nlmsg_len = NLMSG_LENGTH(sizeof(req.ifi));
  req.hdr.nlmsg_type = RTM_GETLINK;
  req.hdr.nlmsg_flags = NLM_F_REQUEST;
  req.ifi.ifi_family = AF_UNSPEC;

  /* Ask by name. Asking by index would answer as well, but the name is what the
   * operator gave, so an error names something they recognise. */
  rta = (struct rtattr*)((char*)&req + NLMSG_ALIGN(req.hdr.nlmsg_len));
  rta->rta_type = IFLA_IFNAME;
  rta->rta_len = (unsigned short)RTA_LENGTH(name_len);
  memcpy(RTA_DATA(rta), ifname, name_len);
  req.hdr.nlmsg_len = NLMSG_ALIGN(req.hdr.nlmsg_len) + RTA_ALIGN(rta->rta_len);

  if (send(fd, &req, req.hdr.nlmsg_len, 0) < 0) {
    int saved = errno;

    close(fd);
    return -saved;
  }

  len = recv(fd, reply, sizeof(reply), 0);
  close(fd);
  if (len < 0) return -errno;

  for (nh = (struct nlmsghdr*)reply; NLMSG_OK(nh, (size_t)len);
       nh = NLMSG_NEXT(nh, len)) {
    struct ifinfomsg* ifi;
    int attr_len;

    if (nh->nlmsg_type == NLMSG_ERROR) return -EIO;
    if (nh->nlmsg_type != RTM_NEWLINK) continue;

    ifi = (struct ifinfomsg*)NLMSG_DATA(nh);
    attr_len = (int)(nh->nlmsg_len - NLMSG_LENGTH(sizeof(*ifi)));

    for (rta = IFLA_RTA(ifi); RTA_OK(rta, attr_len); rta = RTA_NEXT(rta, attr_len)) {
      struct rtattr* sub;
      int sub_len;

      if (rta->rta_type != IFLA_XDP) continue;

      sub_len = (int)RTA_PAYLOAD(rta);
      for (sub = (struct rtattr*)RTA_DATA(rta); RTA_OK(sub, sub_len);
           sub = RTA_NEXT(sub, sub_len)) {
        if (sub->rta_type != IFLA_XDP_PROG_ID) continue;
        if (RTA_PAYLOAD(sub) < sizeof(uint32_t)) continue;
        return (long)*(const uint32_t*)RTA_DATA(sub);
      }
    }

    /* The link answered and named no program. */
    return 0;
  }

  return -ENODATA;
}

/** Whether `fd` is a BPF map. The kernel names the anonymous inode. */
static bool fd_is_bpf_map(int fd) {
  char link[64];
  char target[128];
  ssize_t len;

  snprintf(link, sizeof(link), "/proc/self/fd/%d", fd);
  len = readlink(link, target, sizeof(target) - 1);
  if (len < 0) return false;
  target[len] = '\0';

  return strcmp(target, "anon_inode:bpf-map") == 0;
}

static void group_xdp(void) {
  mtlm_client* holder = NULL;
  mtlm_client* client;
  int first;
  int second;

  g_group = "xdp";

  /* Asking for the map needs no registration, because a client asks for it
   * before it has anything else to say. */
  client = mtlm_client_create(g_sock_path);
  if (!check_true("a client connects", client != NULL, "%s", strerror(errno))) return;

  check_eq("a map of an unknown interface", mtlm_xsk_map_fd(client, 0x00ffffffu),
           -ENOTSUP);
  check_eq("and the connection survives", mtlm_heartbeat(client, 9, NULL), 0);

  if (g_ifindex == 0) {
    skip("no interface to ask for a map", "%s", "give --ifname");
    mtlm_client_destroy(client);
    return;
  }

  /* An unregistered client can read the map, but only a registered one makes the
   * manager load the program, so one of each is needed here. */
  holder = new_registered();
  if (!check_true("a registered client holds the interface", holder != NULL,
                  "could not register")) {
    mtlm_client_destroy(client);
    return;
  }

  first = mtlm_xsk_map_fd(client, g_ifindex);
  if (first < 0) {
    skip("no xsks map for this interface",
         "%s reports %s, and the XDP attach reported %s", g_ifname, ret_text(first),
         ret_text(g_xdp_attach_ret));
    check_eq("and the connection survives", mtlm_heartbeat(client, 10, NULL), 0);
    mtlm_client_destroy(holder);
    mtlm_client_destroy(client);
    return;
  }

  check_ge("a descriptor arrives", first, 0);
  check_true("it is a BPF map", fd_is_bpf_map(first),
             "/proc/self/fd/%d does not name one", first);

  /* Each request is a new descriptor of the same map, because SCM_RIGHTS makes
   * a copy. A client that asks twice must get two it can close on its own. */
  second = mtlm_xsk_map_fd(client, g_ifindex);
  if (check_ge("a second request also gives one", second, 0)) {
    check_true("and it is a different descriptor", second != first, "both are %d", first);
    check_true("which is also a BPF map", fd_is_bpf_map(second), "it is not");
    check_num("closing it works", (long)close(second), 0);
  }

  /* The descriptor belongs to this process now, so the manager going away must
   * not take it with it. That is what lets an instance outlive a restart of the
   * manager. */
  mtlm_client_destroy(client);
  mtlm_client_destroy(holder);
  check_true("the descriptor outlives the connection", fd_is_bpf_map(first),
             "it went away with the client");
  check_num("closing it works", (long)close(first), 0);

  /* The last user of the interface is gone, so the program must be gone too.
   * Read it from sysfs and not from the manager, because the point is what the
   * interface looks like to everything else on the host. A program left behind
   * keeps the interface in XDP mode and stops the next program from taking it.
   * The manager needs a moment to notice the closed sockets. */
  {
    int waited;
    long prog_id = -1;

    for (waited = 0; waited < PROBE_RECLAIM_TIMEOUT_MS; waited += 50) {
      prog_id = if_xdp_prog_id(g_ifname);
      if (prog_id == 0) break;
      sleep_ms(50);
    }
    check_num("no XDP program is left on the interface", prog_id, 0);
  }

  check_true("the manager still serves", manager_serving(), "it does not");
}

/*
 * Group: abuse. Records the client library would never build.
 *
 * Every case ends with a check that the manager still serves. That is the
 * point: a hostile or broken client must not be able to stop the daemon that
 * every other instance on the host depends on.
 */

/** A connected client whose socket this group writes to directly. */
static mtlm_client* abuse_open(int* fd) {
  mtlm_client* client = mtlm_client_create(g_sock_path);

  *fd = client == NULL ? -1 : mtlm_client_fd(client);
  return client;
}

static void abuse_case_end(const char* name, mtlm_client* client) {
  if (client != NULL) mtlm_client_destroy(client);
  check_true(name, manager_serving(), "the manager stopped serving");
}

static void group_abuse(void) {
  mtl_message_t msg;
  mtlm_client* client;
  int fd;
  int ret;

  g_group = "abuse";

  if (!check_true("a manager answers", mtlm_manager_alive(g_sock_path),
                  "nothing answers"))
    return;

  /* A stray byte in front of a whole record. The manager buffers until a whole
   * record is present, so it must answer once the record completes. */
  client = abuse_open(&fd);
  if (client != NULL) {
    char stray = 0x7f;

    wire_init(&msg, MTL_MSG_TYPE_HEARTBEAT, sizeof(mtl_heartbeat_message_t));
    msg.body.heartbeat_msg.seq = htonl(0x11111111u);
    wire_send(fd, &stray, 1);
    wire_send(fd, &msg, MTL_MANAGER_MSG_SIZE);
    /* The stray byte becomes the first byte of the record the manager sees, so
     * it reads a record with the wrong magic and drops the connection. That is
     * the documented answer, and it must not be a crash. */
    ret = wire_read(fd, &msg, PROBE_REPLY_TIMEOUT_MS);
    check_true("a stray byte does not hang the manager", ret != -ETIMEDOUT,
               "nothing came back at all");
  }
  abuse_case_end("the manager serves after a stray byte", client);

  /* One record in two writes, with a pause between them. The manager must keep
   * the first half until the second arrives. */
  client = abuse_open(&fd);
  if (client != NULL) {
    wire_init(&msg, MTL_MSG_TYPE_HEARTBEAT, sizeof(mtl_heartbeat_message_t));
    msg.body.heartbeat_msg.seq = htonl(0x22222222u);
    wire_send(fd, &msg, 5);
    sleep_ms(60);
    wire_send(fd, (const char*)&msg + 5, MTL_MANAGER_MSG_SIZE - 5);
    if (check_eq("a split record is put back together",
                 wire_read(fd, &msg, PROBE_REPLY_TIMEOUT_MS), 0))
      check_num("and the sequence number is right",
                (long)ntohl(msg.body.heartbeat_msg.seq), (long)0x22222222u);
  }
  abuse_case_end("the manager serves after a split record", client);

  /* Two records in one write. The manager must consume every whole record it
   * has, not one per read. */
  client = abuse_open(&fd);
  if (client != NULL) {
    mtl_message_t pair[2];

    wire_init(&pair[0], MTL_MSG_TYPE_HEARTBEAT, sizeof(mtl_heartbeat_message_t));
    pair[0].body.heartbeat_msg.seq = htonl(0x33333333u);
    pair[1] = pair[0];
    pair[1].body.heartbeat_msg.seq = htonl(0x44444444u);
    wire_send(fd, pair, sizeof(pair));
    if (check_eq("the first of two records is answered",
                 wire_read(fd, &msg, PROBE_REPLY_TIMEOUT_MS), 0))
      check_num("with its own sequence number", (long)ntohl(msg.body.heartbeat_msg.seq),
                (long)0x33333333u);
    if (check_eq("the second is answered too",
                 wire_read(fd, &msg, PROBE_REPLY_TIMEOUT_MS), 0))
      check_num("with its own sequence number", (long)ntohl(msg.body.heartbeat_msg.seq),
                (long)0x44444444u);
  }
  abuse_case_end("the manager serves after two records in one write", client);

  /* More records in one write than the read chunk of the manager holds. The
   * last one lands in two reads. */
  client = abuse_open(&fd);
  if (client != NULL) {
    static mtl_message_t burst[PROBE_BURST_RECORDS];
    unsigned int i;
    unsigned int answered = 0;

    for (i = 0; i < PROBE_BURST_RECORDS; i++) {
      wire_init(&burst[i], MTL_MSG_TYPE_HEARTBEAT, sizeof(mtl_heartbeat_message_t));
      burst[i].body.heartbeat_msg.seq = htonl(i);
    }
    if (wire_send(fd, burst, sizeof(burst)) == 0) {
      for (i = 0; i < PROBE_BURST_RECORDS; i++) {
        if (wire_read(fd, &msg, PROBE_REPLY_TIMEOUT_MS) < 0) break;
        if (ntohl(msg.body.heartbeat_msg.seq) != i) break;
        answered++;
      }
    }
    check_num("every record of a burst larger than one read is answered", (long)answered,
              (long)PROBE_BURST_RECORDS);
  }
  abuse_case_end("the manager serves after a burst", client);

  /* The wrong magic. The manager drops the connection, which is the documented
   * answer, and keeps running for everyone else. */
  client = abuse_open(&fd);
  if (client != NULL) {
    wire_init(&msg, MTL_MSG_TYPE_HEARTBEAT, sizeof(mtl_heartbeat_message_t));
    msg.header.magic = htonl(0xdeadbeefu);
    wire_send(fd, &msg, MTL_MANAGER_MSG_SIZE);
    ret = wire_read(fd, &msg, PROBE_REPLY_TIMEOUT_MS);
    check_true("a record with the wrong magic closes the connection",
               ret == -ECONNRESET || ret == -EPIPE, "it reported %s", ret_text(ret));
  }
  abuse_case_end("the manager serves after the wrong magic", client);

  /* A type the manager does not know. It must answer, or a client that waits
   * for a response waits for ever. */
  client = abuse_open(&fd);
  if (client != NULL) {
    wire_init(&msg, 54321, 0);
    wire_send(fd, &msg, MTL_MANAGER_MSG_SIZE);
    if (check_eq("an unknown type is answered",
                 wire_read(fd, &msg, PROBE_REPLY_TIMEOUT_MS), 0))
      check_eq("with not supported", wire_response(&msg), -ENOTSUP);
  }
  abuse_case_end("the manager serves after an unknown type", client);

  /* A type of the other direction, sent by the client. */
  client = abuse_open(&fd);
  if (client != NULL) {
    wire_init(&msg, MTL_MSG_TYPE_RESPONSE, sizeof(mtl_response_message_t));
    wire_send(fd, &msg, MTL_MANAGER_MSG_SIZE);
    if (check_eq("a server to client type is answered",
                 wire_read(fd, &msg, PROBE_REPLY_TIMEOUT_MS), 0))
      check_eq("with not supported", wire_response(&msg), -ENOTSUP);
  }
  abuse_case_end("the manager serves after a reversed type", client);

  /* A body length that claims the record is enormous. Framing is a fixed size,
   * so the manager must never take this number as a length to read or to
   * allocate. */
  client = abuse_open(&fd);
  if (client != NULL) {
    wire_init(&msg, MTL_MSG_TYPE_HEARTBEAT, 0xffffffffu);
    msg.body.heartbeat_msg.seq = htonl(0x55555555u);
    wire_send(fd, &msg, MTL_MANAGER_MSG_SIZE);
    if (check_eq("a body length of every bit set is ignored",
                 wire_read(fd, &msg, PROBE_REPLY_TIMEOUT_MS), 0))
      check_num("and the record is handled normally",
                (long)ntohl(msg.body.heartbeat_msg.seq), (long)0x55555555u);
  }
  abuse_case_end("the manager serves after a huge body length", client);

  /* A register record that names more interfaces than it can hold. The count is
   * in the record, so only the manager can catch this one. */
  client = abuse_open(&fd);
  if (client != NULL) {
    wire_init(&msg, MTL_MSG_TYPE_REGISTER, sizeof(mtl_register_message_t));
    msg.body.register_msg.num_if = htons(0xffff);
    wire_send(fd, &msg, MTL_MANAGER_MSG_SIZE);
    if (check_eq("a register of 65535 interfaces is answered",
                 wire_read(fd, &msg, PROBE_REPLY_TIMEOUT_MS), 0))
      check_eq("with a bad argument", wire_response(&msg), -EINVAL);
  }
  abuse_case_end("the manager serves after an over long register", client);

  /* Half a record and then a close. The manager must drop the client and not
   * spin on the partial buffer. */
  client = abuse_open(&fd);
  if (client != NULL) {
    wire_init(&msg, MTL_MSG_TYPE_HEARTBEAT, sizeof(mtl_heartbeat_message_t));
    wire_send(fd, &msg, MTL_MANAGER_MSG_SIZE / 2);
    mtlm_client_destroy(client);
    client = NULL;
  }
  abuse_case_end("the manager serves after half a record and a close", client);

  /* A shutdown of the write side. The manager reads end of file. */
  client = abuse_open(&fd);
  if (client != NULL) {
    shutdown(fd, SHUT_WR);
    sleep_ms(50);
  }
  abuse_case_end("the manager serves after a one sided shutdown", client);

  /* A client that is killed while it holds resources. Not a clean destroy: the
   * process goes away with the socket still open, which is what a crash looks
   * like to the manager. */
  {
    pid_t pid = fork();

    if (pid == 0) {
      mtlm_client* child = new_registered();

      if (child != NULL) {
        int id = find_free_lcore(child);

        (void)id;
      }
      /* Wait to be killed, holding everything. */
      for (;;) sleep_ms(1000);
    }
    if (pid > 0) {
      sleep_ms(300);
      kill(pid, SIGKILL);
      waitpid(pid, NULL, 0);
      sleep_ms(300);
      check_true("the manager serves after a client was killed", manager_serving(),
                 "it does not");
    } else {
      skip("could not fork a client to kill", "%s", strerror(errno));
    }
  }

  /* Many connections, opened and dropped without a word. */
  {
    int fds[PROBE_MANY_CLIENTS];
    mtlm_client* clients[PROBE_MANY_CLIENTS];
    unsigned int i;
    unsigned int opened = 0;

    for (i = 0; i < PROBE_MANY_CLIENTS; i++) {
      clients[i] = abuse_open(&fds[i]);
      if (clients[i] == NULL) break;
      opened++;
    }
    for (i = 0; i < opened; i++) mtlm_client_destroy(clients[i]);
    check_num("many silent connections are accepted", (long)opened,
              (long)PROBE_MANY_CLIENTS);
    check_true("the manager serves after many silent connections", manager_serving(),
               "it does not");
  }
}

/*
 * Groups and arguments
 */

struct group_entry {
  const char* name;
  void (*run)(void);
  bool needs_manager;
  const char* what;
};

static const struct group_entry groups[] = {
    {"path", group_path, false, "the socket path rules"},
    {"api", group_api, false, "the shape of the API, and every NULL argument"},
    {"connect", group_connect, true, "connect, register and heartbeat"},
    {"lcore", group_lcore, true, "claim and release an lcore"},
    {"queue", group_queue, true, "reserve and release a receive queue"},
    {"flow", group_flow, true, "insert and delete an ethtool flow rule"},
    {"filter", group_filter, true, "the UDP destination port filter of the XDP program"},
    {"xdp", group_xdp, true, "the xsks map descriptor"},
    {"abuse", group_abuse, true, "records the client library would never build"},
};

#define GROUP_COUNT (sizeof(groups) / sizeof(groups[0]))

static void usage(void) {
  unsigned int i;

  printf("Usage: MtlManagerProbe [options] [group ...]\n");
  printf("\n");
  printf("Test every operation of a running MtlManager through the public API.\n");
  printf("\n");
  printf("Options:\n");
  printf("  --sock-path PATH  Path of the manager socket. Without it the client\n");
  printf("                    search order applies, the same as for the library.\n");
  printf("  --ifname NAME     Interface for the queue, flow, filter and xdp groups.\n");
  printf("                    WARNING: the manager deletes every receive flow rule\n");
  printf("                    of this interface. Use one of your own.\n");
  printf("  --verbose         Print a line for each case that passed as well.\n");
  printf("  --list            Name each group and exit.\n");
  printf("  --help            This text.\n");
  printf("\n");
  printf("Groups, in the order they run. \"all\" is every one of them, and is the\n");
  printf("default:\n");
  for (i = 0; i < GROUP_COUNT; i++)
    printf("  %-8s %s%s\n", groups[i].name, groups[i].what,
           groups[i].needs_manager ? "" : " (needs no manager)");
  printf("\n");
  printf("Exit status: 0 no case failed, 1 a case failed, 2 a usage or\n");
  printf("environment problem. A SKIP is not a failure.\n");
}

int main(int argc, char** argv) {
  bool selected[GROUP_COUNT];
  bool any_selected = false;
  bool need_manager = false;
  unsigned int i;
  int arg;

  memset(selected, 0, sizeof(selected));

  for (arg = 1; arg < argc; arg++) {
    const char* a = argv[arg];

    if (strcmp(a, "--help") == 0 || strcmp(a, "-h") == 0) {
      usage();
      return 0;
    }
    if (strcmp(a, "--verbose") == 0 || strcmp(a, "-v") == 0) {
      g_verbose = true;
      continue;
    }
    if (strcmp(a, "--list") == 0) {
      for (i = 0; i < GROUP_COUNT; i++)
        printf("%-8s %s\n", groups[i].name, groups[i].what);
      return 0;
    }
    if (strcmp(a, "--sock-path") == 0) {
      if (++arg >= argc) {
        fprintf(stderr, "--sock-path needs a path\n");
        return 2;
      }
      g_sock_path = argv[arg];
      continue;
    }
    if (strcmp(a, "--ifname") == 0) {
      if (++arg >= argc) {
        fprintf(stderr, "--ifname needs a name\n");
        return 2;
      }
      g_ifname = argv[arg];
      continue;
    }
    if (strcmp(a, "all") == 0) {
      for (i = 0; i < GROUP_COUNT; i++) selected[i] = true;
      any_selected = true;
      continue;
    }
    if (a[0] == '-') {
      fprintf(stderr, "unknown option %s -- try --help\n", a);
      return 2;
    }

    for (i = 0; i < GROUP_COUNT; i++) {
      if (strcmp(a, groups[i].name) != 0) continue;
      selected[i] = true;
      any_selected = true;
      break;
    }
    if (i == GROUP_COUNT) {
      fprintf(stderr, "unknown group %s -- try --list\n", a);
      return 2;
    }
  }

  if (!any_selected)
    for (i = 0; i < GROUP_COUNT; i++) selected[i] = true;

  if (g_ifname != NULL) {
    g_ifindex = if_nametoindex(g_ifname);
    if (g_ifindex == 0) {
      fprintf(stderr, "no interface named %s\n", g_ifname);
      return 2;
    }
  }

  for (i = 0; i < GROUP_COUNT; i++)
    if (selected[i] && groups[i].needs_manager) need_manager = true;

  printf("MtlManagerProbe, protocol %s, record %u bytes\n", mtlm_proto_version(),
         (unsigned int)MTL_MANAGER_MSG_SIZE);
  printf("  socket    %s\n",
         g_sock_path != NULL
             ? g_sock_path
             : (mtlm_sock_path() != NULL ? mtlm_sock_path() : "(cannot resolve)"));
  printf("  interface %s\n", g_ifname != NULL ? g_ifname : "(none, some cases skip)");
  printf("  euid      %u%s\n", (unsigned int)geteuid(),
         geteuid() == 0 ? "" : " (not root, so the XDP cases may skip)");
  printf("\n");

  if (need_manager && !mtlm_manager_alive(g_sock_path)) {
    fprintf(stderr, "No MtlManager answers on %s.\n",
            g_sock_path != NULL ? g_sock_path : "any candidate path");
    fprintf(stderr, "Start one, or run only the groups that need none:\n");
    fprintf(stderr, "  MtlManagerProbe");
    for (i = 0; i < GROUP_COUNT; i++)
      if (!groups[i].needs_manager) fprintf(stderr, " %s", groups[i].name);
    fprintf(stderr, "\n");
    return 2;
  }

  for (i = 0; i < GROUP_COUNT; i++)
    if (selected[i]) groups[i].run();

  printf("\n%u passed, %u failed, %u skipped\n", g_pass, g_fail, g_skip);
  return g_fail == 0 ? 0 : 1;
}
