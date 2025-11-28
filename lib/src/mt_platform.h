/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2022 Intel Corporation
 */

#ifndef _MT_LIB_PLATFORM_HEAD_H_
#define _MT_LIB_PLATFORM_HEAD_H_

#ifdef WINDOWSENV /* Windows */
#include "win_posix.h"
#else /* Linux */
#include <arpa/inet.h>
#include <net/if.h>
#include <net/if_arp.h>
#include <netinet/udp.h>
#include <numa.h>
#include <poll.h>
#include <pthread.h>
#include <sys/ioctl.h>
#include <sys/shm.h>
#include <sys/socket.h>

#ifdef MTL_HAS_AVX512
#include <immintrin.h>
#endif

#endif /* end of WINDOWSENV */

#ifdef CLOCK_MONOTONIC_RAW
#define MT_CLOCK_MONOTONIC_ID CLOCK_MONOTONIC_RAW
#else
#define MT_CLOCK_MONOTONIC_ID CLOCK_MONOTONIC
#endif

#ifdef WINDOWSENV
typedef unsigned long int nfds_t;
#endif

#ifndef POLLIN /* For windows */
/* There is data to read */
#define POLLIN 0x001
#endif

#ifndef MSG_DONTWAIT /* For windows */
#define MSG_DONTWAIT (0x40)
#endif

#ifdef WINDOWSENV
#define MT_THREAD_TIMEDWAIT_CLOCK_ID CLOCK_REALTIME
#else
/* use CLOCK_MONOTONIC for mt_pthread_cond_timedwait */
#define MT_THREAD_TIMEDWAIT_CLOCK_ID CLOCK_MONOTONIC
#endif

#ifdef WINDOWSENV
#define MT_FLOCK_PATH "c:/temp/kahawai_lcore.lock"
#else
#define MT_FLOCK_PATH "/tmp/kahawai_lcore.lock"
#endif

#ifndef WINDOWSENV
// #define MT_ENABLE_P_SHARED /* default enable PTHREAD_PROCESS_SHARED */
#endif

/* Mutex ownership tracking for debugging */
#include <stdatomic.h>
#include <execinfo.h>

struct mt_mutex_owner {
  atomic_ulong owner_tid;
  atomic_ulong lock_count;
  char owner_func[128];
  char owner_file[256];
  int owner_line;
  void* backtrace_addrs[10];  /* Store backtrace for debugging */
  int backtrace_size;
};

/* Simple hash table for mutex ownership tracking */
#define MT_MUTEX_TRACK_SIZE 1024
static struct {
  pthread_mutex_t* mutex_ptr;
  struct mt_mutex_owner owner;
} mt_mutex_track[MT_MUTEX_TRACK_SIZE];

static inline struct mt_mutex_owner* mt_get_mutex_owner(pthread_mutex_t* mutex) {
  size_t hash = ((size_t)mutex >> 3) % MT_MUTEX_TRACK_SIZE;
  for (size_t i = 0; i < MT_MUTEX_TRACK_SIZE; i++) {
    size_t idx = (hash + i) % MT_MUTEX_TRACK_SIZE;
    if (mt_mutex_track[idx].mutex_ptr == mutex) {
      return &mt_mutex_track[idx].owner;
    }
    if (mt_mutex_track[idx].mutex_ptr == NULL) {
      mt_mutex_track[idx].mutex_ptr = mutex;
      atomic_store(&mt_mutex_track[idx].owner.owner_tid, 0);
      atomic_store(&mt_mutex_track[idx].owner.lock_count, 0);
      return &mt_mutex_track[idx].owner;
    }
  }
  return NULL; /* Hash table full */
}

/* Use macro wrappers to capture caller information */
#define mt_pthread_mutex_init(mutex, attr) \
  mt_pthread_mutex_init_impl(mutex, attr, __func__, __FILE__, __LINE__)

#define mt_pthread_mutex_lock(mutex) \
  mt_pthread_mutex_lock_impl(mutex, __func__, __FILE__, __LINE__)

#define mt_pthread_mutex_try_lock(mutex) \
  mt_pthread_mutex_try_lock_impl(mutex, __func__, __FILE__, __LINE__)

#define mt_pthread_mutex_unlock(mutex) \
  mt_pthread_mutex_unlock_impl(mutex, __func__, __FILE__, __LINE__)

#define mt_pthread_mutex_destroy(mutex) \
  mt_pthread_mutex_destroy_impl(mutex, __func__, __FILE__, __LINE__)

static inline int mt_pthread_mutex_init_impl(pthread_mutex_t* mutex,
                                             pthread_mutexattr_t* p_attr,
                                             const char* caller_func,
                                             const char* caller_file,
                                             int caller_line) {
  fprintf(stderr, "[MUTEX_INIT] mutex=%p thread=%lu caller=%s (%s:%d)\n", 
          (void*)mutex, pthread_self(), caller_func, caller_file, caller_line);
#ifdef MT_ENABLE_P_SHARED
  pthread_mutexattr_t attr;
  if (p_attr) {
    pthread_mutexattr_setpshared(p_attr, PTHREAD_PROCESS_SHARED);
  } else {
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_setpshared(&attr, PTHREAD_PROCESS_SHARED);
    p_attr = &attr;
  }
#endif

  int ret = pthread_mutex_init(mutex, p_attr);
  if (ret == 0) {
    fprintf(stderr, "[MUTEX_INIT] SUCCESS mutex=%p thread=%lu caller=%s\n", 
            (void*)mutex, pthread_self(), caller_func);
  } else {
    fprintf(stderr, "[MUTEX_INIT] FAILED mutex=%p thread=%lu caller=%s ret=%d\n", 
            (void*)mutex, pthread_self(), caller_func, ret);
  }
  return ret;
}

static inline int mt_pthread_mutex_lock_impl(pthread_mutex_t* mutex,
                                             const char* caller_func,
                                             const char* caller_file,
                                             int caller_line) {
  struct mt_mutex_owner* owner = mt_get_mutex_owner(mutex);
  
  fprintf(stderr, "[MUTEX_LOCK] ATTEMPT mutex=%p thread=%lu caller=%s (%s:%d)\n", 
          (void*)mutex, pthread_self(), caller_func, caller_file, caller_line);
  
  /* Check current owner before blocking */
  if (owner) {
    unsigned long current_owner = atomic_load(&owner->owner_tid);
    if (current_owner != 0 && current_owner != pthread_self()) {
      unsigned long lock_count = atomic_load(&owner->lock_count);
      fprintf(stderr, "[MUTEX_LOCK] BLOCKED! mutex=%p is held by thread=%lu (lock_count=%lu) in %s (%s:%d)\n",
              (void*)mutex, current_owner, lock_count,
              owner->owner_func, owner->owner_file, owner->owner_line);
      /* Print backtrace of who holds the lock */
      if (owner->backtrace_size > 0) {
        char** symbols = backtrace_symbols(owner->backtrace_addrs, owner->backtrace_size);
        if (symbols) {
          fprintf(stderr, "[MUTEX_LOCK] OWNER BACKTRACE:\n");
          for (int i = 0; i < owner->backtrace_size && i < 5; i++) {
            fprintf(stderr, "  [%d] %s\n", i, symbols[i]);
          }
          free(symbols);
        }
      }
      /* Print our own backtrace (who is waiting) */
      void* wait_backtrace[10];
      int wait_size = backtrace(wait_backtrace, 10);
      if (wait_size > 0) {
        char** wait_symbols = backtrace_symbols(wait_backtrace, wait_size);
        if (wait_symbols) {
          fprintf(stderr, "[MUTEX_LOCK] WAITER BACKTRACE (thread=%lu):\n", pthread_self());
          for (int i = 0; i < wait_size && i < 5; i++) {
            fprintf(stderr, "  [%d] %s\n", i, wait_symbols[i]);
          }
          free(wait_symbols);
        }
      }
    }
  }
  
  int ret = pthread_mutex_lock(mutex);
  if (ret == 0) {
    /* Record ownership */
    if (owner) {
      atomic_store(&owner->owner_tid, pthread_self());
      atomic_fetch_add(&owner->lock_count, 1);
      snprintf(owner->owner_func, sizeof(owner->owner_func), "%s", caller_func);
      snprintf(owner->owner_file, sizeof(owner->owner_file), "%s", caller_file);
      owner->owner_line = caller_line;
      /* Capture backtrace */
      owner->backtrace_size = backtrace(owner->backtrace_addrs, 10);
    }
    fprintf(stderr, "[MUTEX_LOCK] ACQUIRED mutex=%p thread=%lu caller=%s\n", 
            (void*)mutex, pthread_self(), caller_func);
  } else {
    fprintf(stderr, "[MUTEX_LOCK] FAILED mutex=%p thread=%lu caller=%s ret=%d\n", 
            (void*)mutex, pthread_self(), caller_func, ret);
  }
  return ret;
}

static inline int mt_pthread_mutex_try_lock_impl(pthread_mutex_t* mutex,
                                                 const char* caller_func,
                                                 const char* caller_file,
                                                 int caller_line) {
  struct mt_mutex_owner* owner = mt_get_mutex_owner(mutex);
  
  fprintf(stderr, "[MUTEX_TRYLOCK] ATTEMPT mutex=%p thread=%lu caller=%s (%s:%d)\n", 
          (void*)mutex, pthread_self(), caller_func, caller_file, caller_line);
  int ret = pthread_mutex_trylock(mutex);
  if (ret == 0) {
    /* Record ownership */
    if (owner) {
      atomic_store(&owner->owner_tid, pthread_self());
      atomic_fetch_add(&owner->lock_count, 1);
      snprintf(owner->owner_func, sizeof(owner->owner_func), "%s", caller_func);
      snprintf(owner->owner_file, sizeof(owner->owner_file), "%s", caller_file);
      owner->owner_line = caller_line;
      /* Capture backtrace */
      owner->backtrace_size = backtrace(owner->backtrace_addrs, 10);
    }
    fprintf(stderr, "[MUTEX_TRYLOCK] ACQUIRED mutex=%p thread=%lu caller=%s\n", 
            (void*)mutex, pthread_self(), caller_func);
  } else if (ret == EBUSY) {
    /* Report who owns it */
    if (owner) {
      unsigned long current_owner = atomic_load(&owner->owner_tid);
      unsigned long lock_count = atomic_load(&owner->lock_count);
      if (current_owner != 0) {
        fprintf(stderr, "[MUTEX_TRYLOCK] BUSY mutex=%p thread=%lu caller=%s - HELD BY thread=%lu (lock_count=%lu) in %s (%s:%d)\n", 
                (void*)mutex, pthread_self(), caller_func,
                current_owner, lock_count,
                owner->owner_func, owner->owner_file, owner->owner_line);
        /* Print backtrace of who holds the lock */
        if (owner->backtrace_size > 0) {
          char** symbols = backtrace_symbols(owner->backtrace_addrs, owner->backtrace_size);
          if (symbols) {
            fprintf(stderr, "[MUTEX_TRYLOCK] OWNER BACKTRACE:\n");
            for (int i = 0; i < owner->backtrace_size && i < 5; i++) {
              fprintf(stderr, "  [%d] %s\n", i, symbols[i]);
            }
            free(symbols);
          }
        }
      } else {
        fprintf(stderr, "[MUTEX_TRYLOCK] BUSY mutex=%p thread=%lu caller=%s\n", 
                (void*)mutex, pthread_self(), caller_func);
      }
    } else {
      fprintf(stderr, "[MUTEX_TRYLOCK] BUSY mutex=%p thread=%lu caller=%s\n", 
              (void*)mutex, pthread_self(), caller_func);
    }
  } else {
    fprintf(stderr, "[MUTEX_TRYLOCK] FAILED mutex=%p thread=%lu caller=%s ret=%d\n", 
            (void*)mutex, pthread_self(), caller_func, ret);
  }
  return ret;
}

static inline int mt_pthread_mutex_unlock_impl(pthread_mutex_t* mutex,
                                               const char* caller_func,
                                               const char* caller_file,
                                               int caller_line) {
  struct mt_mutex_owner* owner = mt_get_mutex_owner(mutex);
  
  fprintf(stderr, "[MUTEX_UNLOCK] mutex=%p thread=%lu caller=%s (%s:%d)\n", 
          (void*)mutex, pthread_self(), caller_func, caller_file, caller_line);
  
  /* Clear ownership before unlocking */
  if (owner) {
    unsigned long current_owner = atomic_load(&owner->owner_tid);
    if (current_owner != pthread_self()) {
      fprintf(stderr, "[MUTEX_UNLOCK] WARNING! thread=%lu unlocking mutex=%p owned by thread=%lu\n",
              pthread_self(), (void*)mutex, current_owner);
    }
    atomic_store(&owner->owner_tid, 0);
  }
  
  int ret = pthread_mutex_unlock(mutex);
  if (ret != 0) {
    fprintf(stderr, "[MUTEX_UNLOCK] FAILED mutex=%p thread=%lu caller=%s ret=%d\n", 
            (void*)mutex, pthread_self(), caller_func, ret);
  }
  return ret;
}

static inline int mt_pthread_mutex_destroy_impl(pthread_mutex_t* mutex,
                                                const char* caller_func,
                                                const char* caller_file,
                                                int caller_line) {
  fprintf(stderr, "[MUTEX_DESTROY] mutex=%p thread=%lu caller=%s (%s:%d)\n", 
          (void*)mutex, pthread_self(), caller_func, caller_file, caller_line);
  int ret = pthread_mutex_destroy(mutex);
  if (ret != 0) {
    fprintf(stderr, "[MUTEX_DESTROY] FAILED mutex=%p thread=%lu caller=%s ret=%d\n", 
            (void*)mutex, pthread_self(), caller_func, ret);
  }
  return ret;
}

static inline int mt_pthread_cond_init(pthread_cond_t* cond,
                                       pthread_condattr_t* cond_attr) {
  return pthread_cond_init(cond, cond_attr);
}

static inline int mt_pthread_cond_wait(pthread_cond_t* cond, pthread_mutex_t* mutex) {
  return pthread_cond_wait(cond, mutex);
}

static inline int mt_pthread_cond_timedwait(pthread_cond_t* cond, pthread_mutex_t* mutex,
                                            const struct timespec* time) {
  return pthread_cond_timedwait(cond, mutex, time);
}

static inline int mt_pthread_cond_destroy(pthread_cond_t* cond) {
  return pthread_cond_destroy(cond);
}

static inline int mt_pthread_cond_wait_init(pthread_cond_t* cond) {
#if MT_THREAD_TIMEDWAIT_CLOCK_ID != CLOCK_REALTIME
  pthread_condattr_t attr;
  pthread_condattr_init(&attr);
  pthread_condattr_setclock(&attr, MT_THREAD_TIMEDWAIT_CLOCK_ID);
  return mt_pthread_cond_init(cond, &attr);
#else
  return mt_pthread_cond_init(cond, NULL);
#endif
}

static inline void timespec_add_ns(struct timespec* time, uint64_t ns) {
  time->tv_nsec += ns;
  while (time->tv_nsec >= 1000000000L) {
    time->tv_nsec -= 1000000000L;
    time->tv_sec++;
  }
}

static inline int mt_pthread_cond_timedwait_ns(pthread_cond_t* cond,
                                               pthread_mutex_t* mutex,
                                               uint64_t timedwait_ns) {
  struct timespec time;
  clock_gettime(MT_THREAD_TIMEDWAIT_CLOCK_ID, &time);
  timespec_add_ns(&time, timedwait_ns);
  return mt_pthread_cond_timedwait(cond, mutex, &time);
}

static inline int mt_pthread_cond_signal(pthread_cond_t* cond) {
  return pthread_cond_signal(cond);
}

static inline bool mt_socket_match(int cpu_socket, int dev_socket) {
#ifdef WINDOWSENV
  MTL_MAY_UNUSED(cpu_socket);
  MTL_MAY_UNUSED(dev_socket);
  return true;  // windows cpu socket always 0
#else
  return (cpu_socket == dev_socket);
#endif
}
#endif
