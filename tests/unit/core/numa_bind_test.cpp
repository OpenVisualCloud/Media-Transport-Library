/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 *
 * mtl_init() binds the calling process to the numa node its primary port sits
 * on. It must narrow the cpu affinity to that node, never widen it back out --
 * see mt_bind_process_numa() for why. Widening turned
 * St20_rx.digest_ooo_slice_4320p red on an isolcpus runner and nowhere else.
 *
 * Two of the four cases need a second numa node and skip without one, so on the
 * single-node image the unit workflow runs, a green CI gates the no-widening
 * case and the range guard only. Read a verdict here as the whole contract only
 * on a multi-node host.
 */

#include <gtest/gtest.h>
#include <numa.h>

#include <cerrno>
#include <vector>

#include "core/mt_numa_harness.h"

namespace {

/* A libnuma cpu mask that frees itself, so a failed assertion cannot leak it. */
class CpuMask {
 public:
  CpuMask() : mask_(numa_allocate_cpumask()) {
  }
  ~CpuMask() {
    numa_bitmask_free(mask_);
  }
  CpuMask(const CpuMask&) = delete;
  CpuMask& operator=(const CpuMask&) = delete;

  bitmask* get() const {
    return mask_;
  }
  bool has(unsigned int cpu) const {
    return numa_bitmask_isbitset(mask_, cpu);
  }
  unsigned int size() const {
    return mask_->size;
  }
  unsigned int weight() const {
    return numa_bitmask_weight(mask_);
  }
  std::vector<int> cpus() const {
    std::vector<int> out;
    for (unsigned int cpu = 0; cpu < size(); cpu++)
      if (has(cpu)) out.push_back((int)cpu);
    return out;
  }

 private:
  bitmask* mask_;
};

class MtNumaBindTest : public testing::Test {
 protected:
  void SetUp() override {
    if (numa_available() < 0) GTEST_SKIP() << "libnuma reports no numa support";
    ASSERT_GE(numa_sched_getaffinity(0, saved_cpus_.get()), 0);
    saved_cpus_valid_ = true;
    saved_membind_ = numa_get_membind();
  }

  void TearDown() override {
    /* Every other case in this binary runs on this same thread, and an EAL init
     * in one of them would take its lcore set from whatever mask is left here.
     * Put back exactly what was found. */
    if (saved_cpus_valid_) numa_sched_setaffinity(0, saved_cpus_.get());
    /* still null when SetUp skipped or asserted before getting this far --
     * TearDown runs either way, and numa_set_membind(nullptr) segfaults. */
    if (saved_membind_) {
      numa_set_membind(saved_membind_);
      numa_bitmask_free(saved_membind_);
    }
  }

  /* Runs this thread on every cpu of `node` the host will actually allow, and
   * reports what that turned out to be. False when the node has none. */
  bool RunOnNode(int node, CpuMask* granted) {
    CpuMask wanted;
    if (numa_node_to_cpus(node, wanted.get()) < 0) return false;
    if (!wanted.weight()) return false;
    if (numa_sched_setaffinity(0, wanted.get()) < 0) return false; /* offline cpus */
    if (numa_sched_getaffinity(0, granted->get()) < 0) return false;
    return granted->weight() > 0;
  }

  CpuMask saved_cpus_;
  bool saved_cpus_valid_ = false;
  bitmask* saved_membind_ = nullptr;
};

/* The regression. A cpu withheld from this thread must not come back. */
TEST_F(MtNumaBindTest, NeverAddsACpuTheCallerWasNotAllowedToUse) {
  int node = -1;
  CpuMask node_cpus;
  for (int n = 0; n <= numa_max_node(); n++) {
    if (RunOnNode(n, &node_cpus) && node_cpus.weight() >= 2) {
      node = n;
      break;
    }
  }
  if (node < 0) GTEST_SKIP() << "no numa node offers 2 cpus this thread may use";

  /* Hand over every cpu of the node but one. */
  const int withheld = node_cpus.cpus().back();
  CpuMask given;
  for (int cpu : node_cpus.cpus())
    if (cpu != withheld) numa_bitmask_setbit(given.get(), (unsigned int)cpu);
  ASSERT_GE(numa_sched_setaffinity(0, given.get()), 0);

  ASSERT_EQ(ut_bind_process_numa(node), 0);

  CpuMask got;
  ASSERT_GE(numa_sched_getaffinity(0, got.get()), 0);
  EXPECT_FALSE(got.has((unsigned int)withheld))
      << "cpu " << withheld << " was withheld from this thread but numa node " << node
      << " binding handed it back";
  for (int cpu : got.cpus())
    EXPECT_TRUE(given.has((unsigned int)cpu)) << "cpu " << cpu << " was not on offer";
  EXPECT_GT(got.weight(), 0u) << "binding left this thread with no cpu at all";

  /* The memory half of the binding is the point of doing this at all. */
  bitmask* membind = numa_get_membind();
  EXPECT_TRUE(numa_bitmask_isbitset(membind, (unsigned int)node));
  EXPECT_EQ(numa_bitmask_weight(membind), 1u);
  numa_bitmask_free(membind);
}

/* A healthy host: nothing of the node is withheld, so all of it has to be
 * applied -- and nothing outside it. Entering with one foreign cpu in the mask
 * is what makes this fail for a binding that narrows too far and for one that
 * does nothing at all. */
TEST_F(MtNumaBindTest, AppliesTheWholeNodeAndOnlyTheNode) {
  if (numa_max_node() < 1) GTEST_SKIP() << "single numa node host";

  int node = -1;
  CpuMask node_cpus;
  for (int n = 0; n <= numa_max_node(); n++) {
    if (RunOnNode(n, &node_cpus)) {
      node = n;
      break;
    }
  }
  if (node < 0) GTEST_SKIP() << "no numa node offers a cpu this thread may use";

  int foreign = -1;
  for (int n = 0; n <= numa_max_node() && foreign < 0; n++) {
    if (n == node) continue;
    CpuMask other;
    if (numa_node_to_cpus(n, other.get()) < 0) continue;
    for (int cpu : other.cpus())
      if (saved_cpus_.has((unsigned int)cpu)) {
        foreign = cpu;
        break;
      }
  }
  if (foreign < 0) GTEST_SKIP() << "no cpu off this node that this thread may use";

  CpuMask entry;
  for (int cpu : node_cpus.cpus()) numa_bitmask_setbit(entry.get(), (unsigned int)cpu);
  numa_bitmask_setbit(entry.get(), (unsigned int)foreign);
  ASSERT_GE(numa_sched_setaffinity(0, entry.get()), 0);

  ASSERT_EQ(ut_bind_process_numa(node), 0);

  CpuMask got;
  ASSERT_GE(numa_sched_getaffinity(0, got.get()), 0);
  EXPECT_EQ(got.cpus(), node_cpus.cpus())
      << "expected all of numa node " << node << " and not cpu " << foreign;
}

/* A caller placed on another socket on purpose keeps its cpus. Stranding it on
 * a node it may not run on would be worse than not binding at all. */
TEST_F(MtNumaBindTest, KeepsTheAffinityWhenTheNodeOffersNoUsableCpu) {
  if (numa_max_node() < 1) GTEST_SKIP() << "single numa node host";

  int home = -1, other = -1;
  CpuMask home_cpus;
  for (int n = 0; n <= numa_max_node() && home < 0; n++)
    if (RunOnNode(n, &home_cpus)) home = n;
  if (home < 0) GTEST_SKIP() << "no numa node offers a cpu this thread may use";

  CpuMask other_cpus;
  for (int n = 0; n <= numa_max_node(); n++) {
    if (n == home) continue;
    if (numa_node_to_cpus(n, other_cpus.get()) < 0) continue;
    if (other_cpus.weight()) {
      other = n;
      break;
    }
  }
  if (other < 0) GTEST_SKIP() << "no second numa node with cpus";

  /* Back onto the home node only, then ask for the other one. */
  ASSERT_GE(numa_sched_setaffinity(0, home_cpus.get()), 0);
  EXPECT_EQ(ut_bind_process_numa(other), 0);

  CpuMask got;
  ASSERT_GE(numa_sched_getaffinity(0, got.get()), 0);
  EXPECT_EQ(got.cpus(), home_cpus.cpus());

  /* The memory half still applies -- that is the half worth keeping here. */
  bitmask* membind = numa_get_membind();
  EXPECT_TRUE(numa_bitmask_isbitset(membind, (unsigned int)other));
  EXPECT_EQ(numa_bitmask_weight(membind), 1u);
  numa_bitmask_free(membind);
}

/* MTL_PORT_FLAG_FORCE_NUMA hands over whatever socket_id the caller put in
 * mtl_init_params, and nothing validates it on the way here. */
TEST_F(MtNumaBindTest, RejectsANodeThatDoesNotExist) {
  /* The negative half is the one only MTL can catch: libnuma range-checks a node
   * above numa_max_node() itself, but for a negative one numa_node_to_cpus()
   * indexes its node_cpu_mask array out of bounds. */
  EXPECT_EQ(ut_bind_process_numa(-1), -EINVAL);
  EXPECT_EQ(ut_bind_process_numa(numa_max_node() + 1), -EINVAL);

  CpuMask got;
  ASSERT_GE(numa_sched_getaffinity(0, got.get()), 0);
  EXPECT_EQ(got.cpus(), saved_cpus_.cpus()) << "a rejected node still moved this thread";
}

}  // namespace
