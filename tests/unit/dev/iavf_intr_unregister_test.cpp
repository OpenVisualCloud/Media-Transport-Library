/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 */

#include <elf.h>
#include <errno.h>
#include <fcntl.h>
#include <gtest/gtest.h>
#include <link.h>
#include <rte_interrupts.h>
#include <rte_version.h>
#include <semaphore.h>
#include <sys/eventfd.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <atomic>
#include <cstring>
#include <set>
#include <string>
#include <thread>

#include "common/ut_common.h"

namespace {

int find_iavf(struct dl_phdr_info* info, size_t, void* data) {
  if (info->dlpi_name && strstr(info->dlpi_name, "librte_net_iavf.so")) {
    *static_cast<std::string*>(data) = info->dlpi_name;
    return 1;
  }
  return 0;
}

std::set<std::string> undefined_dynamic_symbols(const std::string& path) {
  std::set<std::string> names;
  int fd = open(path.c_str(), O_RDONLY);
  if (fd < 0) return names;
  struct stat st;
  if (fstat(fd, &st) < 0) {
    close(fd);
    return names;
  }
  void* map = mmap(nullptr, st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
  close(fd);
  if (map == MAP_FAILED) return names;

  auto* base = static_cast<const uint8_t*>(map);
  auto* ehdr = reinterpret_cast<const Elf64_Ehdr*>(base);
  auto* shdrs = reinterpret_cast<const Elf64_Shdr*>(base + ehdr->e_shoff);
  for (int i = 0; i < ehdr->e_shnum; i++) {
    if (shdrs[i].sh_type != SHT_DYNSYM) continue;
    auto* syms = reinterpret_cast<const Elf64_Sym*>(base + shdrs[i].sh_offset);
    auto* strtab =
        reinterpret_cast<const char*>(base + shdrs[shdrs[i].sh_link].sh_offset);
    for (size_t s = 0; s < shdrs[i].sh_size / sizeof(Elf64_Sym); s++)
      if (syms[s].st_shndx == SHN_UNDEF && syms[s].st_name)
        names.insert(strtab + syms[s].st_name);
  }
  munmap(map, st.st_size);
  return names;
}

bool sem_wait_ms(sem_t* sem, long ms) {
  struct timespec deadline;
  clock_gettime(CLOCK_REALTIME, &deadline);
  deadline.tv_sec += ms / 1000;
  deadline.tv_nsec += (ms % 1000) * 1000000L;
  if (deadline.tv_nsec >= 1000000000L) {
    deadline.tv_sec++;
    deadline.tv_nsec -= 1000000000L;
  }
  while (sem_timedwait(sem, &deadline) < 0)
    if (errno != EINTR) return false;
  return true;
}

}  // namespace

/* The EAL interrupt thread stays in held_callback() until the test releases it. */
class EalIntrCallback : public ::testing::Test {
 protected:
  EalIntrCallback() {
    sem_init(&entered_, 0, 0);
    sem_init(&release_, 0, 0);
  }

  ~EalIntrCallback() override {
    sem_destroy(&release_);
    sem_destroy(&entered_);
  }

  static void held_callback(void* arg) {
    auto* self = static_cast<EalIntrCallback*>(arg);
    sem_post(&self->entered_);
    sem_wait(&self->release_);
    self->callback_returned_ = true;
  }

  void SetUp() override {
    ASSERT_EQ(ut_eal_init(), 0);
    efd_ = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    ASSERT_GE(efd_, 0);
    handle_ = rte_intr_instance_alloc(RTE_INTR_INSTANCE_F_PRIVATE);
    ASSERT_NE(handle_, nullptr);
    ASSERT_EQ(rte_intr_type_set(handle_, RTE_INTR_HANDLE_VFIO_MSIX), 0);
    ASSERT_EQ(rte_intr_fd_set(handle_, efd_), 0);
    ASSERT_EQ(rte_intr_callback_register(handle_, held_callback, this), 0);
  }

  void TearDown() override {
    release_callback();
    if (handle_) rte_intr_callback_unregister_sync(handle_, held_callback, this);
    rte_intr_instance_free(handle_);
    if (efd_ >= 0) close(efd_);
  }

  void fire_and_hold_callback() {
    uint64_t one = 1;
    ASSERT_EQ(write(efd_, &one, sizeof(one)), (ssize_t)sizeof(one));
    ASSERT_TRUE(sem_wait_ms(&entered_, 5000))
        << "the EAL interrupt thread did not run the callback";
  }

  void release_callback() {
    if (released_) return;
    released_ = true;
    sem_post(&release_);
  }

  sem_t entered_;
  sem_t release_;
  bool released_ = false;
  int efd_ = -1;
  struct rte_intr_handle* handle_ = nullptr;
  std::atomic<bool> callback_returned_{false};
};

TEST_F(EalIntrCallback, UnregisterOfRunningCallbackKeepsIt) {
  ASSERT_NO_FATAL_FAILURE(fire_and_hold_callback());

  EXPECT_EQ(rte_intr_callback_unregister(handle_, held_callback, this), -EAGAIN);

  release_callback();
  int ret;
  while ((ret = rte_intr_callback_unregister(handle_, held_callback, this)) == -EAGAIN)
    std::this_thread::yield();
  EXPECT_EQ(ret, 1) << "the callback was not still registered after -EAGAIN";
}

TEST_F(EalIntrCallback, UnregisterSyncWaitsForRunningCallback) {
  ASSERT_NO_FATAL_FAILURE(fire_and_hold_callback());

  sem_t sync_returned;
  ASSERT_EQ(sem_init(&sync_returned, 0, 0), 0);
  int sync_ret = 0;
  bool callback_returned_first = false;
  std::thread closer([&] {
    sync_ret = rte_intr_callback_unregister_sync(handle_, held_callback, this);
    callback_returned_first = callback_returned_;
    sem_post(&sync_returned);
  });
  /* A sync call that does not wait returns in this window, before the release. */
  sem_wait_ms(&sync_returned, 100);
  release_callback();
  closer.join();
  sem_destroy(&sync_returned);

  EXPECT_EQ(sync_ret, 1);
  EXPECT_TRUE(callback_returned_first)
      << "rte_intr_callback_unregister_sync() returned before the callback did";
  EXPECT_EQ(rte_intr_callback_unregister(handle_, held_callback, this), -ENOENT)
      << "the interrupt source outlived rte_intr_callback_unregister_sync()";
}

class DpdkIavfPatch : public ::testing::Test {
 protected:
  void SetUp() override {
    if (RTE_VERSION < RTE_VERSION_NUM(26, 7, 0, 0) || !strstr(rte_version(), "_mtl_"))
      GTEST_SKIP() << rte_version() << " is not an MTL-patched DPDK 26.07 or later";
    ASSERT_EQ(ut_eal_init(), 0);

    dl_iterate_phdr(find_iavf, &iavf_);
    if (iavf_.empty()) GTEST_SKIP() << "no iavf PMD plugin loaded by rte_eal_init()";

    imports_ = undefined_dynamic_symbols(iavf_);
    ASSERT_FALSE(imports_.empty()) << "cannot read the dynamic symbols of " << iavf_;
  }

  static std::string rebuild(const std::string& md) {
    return "; rebuild DPDK with patches/dpdk/26.07 (script/build_dpdk.sh), see "
           "patches/dpdk/26.07/" +
           md;
  }

  std::string iavf_;
  std::set<std::string> imports_;
};

/* A lost -EAGAIN leaves a stale source that panics the EAL intr thread at
 * rte_eal_cleanup(). */
TEST_F(DpdkIavfPatch, CloseWaitsForInFlightInterruptCallback) {
  const std::string md = rebuild("0009-net-iavf-fix-interrupt-callback-race-on-close.md");
  EXPECT_TRUE(imports_.count("rte_intr_callback_unregister_sync"))
      << iavf_ << " lacks patch 0009" << md;
  EXPECT_FALSE(imports_.count("rte_intr_callback_unregister"))
      << iavf_ << " still unregisters an interrupt callback without waiting for it" << md;
}

/* Only the event-thread check of iavf_dev_event_discard() imports them in iavf. */
TEST_F(DpdkIavfPatch, CloseWaitsForInFlightEventOfThePort) {
  const std::string md = rebuild("0017-net-iavf-fix-event-use-after-release-on-close.md");
  EXPECT_TRUE(imports_.count("rte_thread_self") && imports_.count("rte_thread_equal"))
      << iavf_ << " lacks patch 0017: the iavf-event thread can handle an event of a "
      << "port after rte_eth_dev_release_port()" << md;
}
