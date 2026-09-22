/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2023 Intel Corporation
 */

/* The queue accounting, the flow accounting and the interface registry. */

#include <gtest/gtest.h>

#include <cerrno>
#include <memory>
#include <stdexcept>

#include "mtl_interface.hpp"
#include "mtlm_test_fakes.hpp"

namespace {

/* One interface over a fake device, with the fakes kept for the checks. */
struct fixture {
  std::shared_ptr<fake_netdev> netdev = std::make_shared<fake_netdev>();
  std::shared_ptr<fake_xdp_state> xdp = std::make_shared<fake_xdp_state>();

  std::unique_ptr<mtl_interface> make(unsigned int ifindex, bool require_xdp = false) {
    return std::unique_ptr<mtl_interface>(new mtl_interface(
        ifindex, netdev, std::unique_ptr<mtlm_xdp_ops>(new fake_xdp(xdp)), require_xdp));
  }
};

} /* namespace */

TEST(RuleLocation, PicksTheLowestPriorityFreeSlot) {
  std::vector<uint32_t> used;

  /* The table runs from 0, the highest priority, upward. The manager takes the
   * last slot so an operator rule keeps winning. */
  EXPECT_EQ(mtlm_pick_free_rule_location(used, 8), 7);

  used.push_back(7);
  EXPECT_EQ(mtlm_pick_free_rule_location(used, 8), 6);
}

TEST(RuleLocation, NeverPicksLocationZero) {
  std::vector<uint32_t> used = {1, 2, 3};

  /* Location 0 stays free on purpose. */
  EXPECT_EQ(mtlm_pick_free_rule_location(used, 4), -ENOSPC);
}

TEST(RuleLocation, ATableOfOneHoldsNoManagerRule) {
  std::vector<uint32_t> used;

  EXPECT_EQ(mtlm_pick_free_rule_location(used, 1), -ENOSPC);
}

TEST(RuleLocation, NoTableIsNoSpace) {
  std::vector<uint32_t> used;

  /* The old code read a table size of 0 as location -1 and asked the driver to
   * install a rule there. */
  EXPECT_EQ(mtlm_pick_free_rule_location(used, 0), -ENOSPC);
}

TEST(Interface, UnknownIndexThrows) {
  fixture f;

  EXPECT_THROW(f.make(1), std::runtime_error);
}

TEST(Interface, UnreadableChannelsThrowAndDetachTheProgram) {
  fixture f;

  f.netdev->add_if(1);
  f.netdev->get_channels_ret = -EOPNOTSUPP;

  EXPECT_THROW(f.make(1), std::runtime_error);
  /* An interface that fails to build must leave no program behind. */
  EXPECT_EQ(f.xdp->detach_calls, 1);
  EXPECT_FALSE(f.xdp->attached);
}

TEST(Interface, AFailedAttachIsFatalOnlyWhenTheCallerNeedsXdp) {
  fixture f;

  f.netdev->add_if(1);
  f.xdp->attach_ret = -ENOTSUP;

  /* A build with no libxdp used to make the whole manager useless here. */
  auto queues_only = f.make(1, false);
  ASSERT_NE(queues_only, nullptr);
  EXPECT_FALSE(queues_only->has_xdp());
  EXPECT_EQ(queues_only->get_queue(), 1);

  EXPECT_THROW(f.make(1, true), std::runtime_error);
}

TEST(Interface, QueueZeroStaysWithTheKernel) {
  fixture f;

  f.netdev->add_if(1, 4);
  auto interface = f.make(1);

  EXPECT_EQ(interface->queue_count(), 4u);
  EXPECT_EQ(interface->get_queue(), 1);
  EXPECT_EQ(interface->get_queue(), 2);
  EXPECT_EQ(interface->get_queue(), 3);
  EXPECT_EQ(interface->get_queue(), -ENOSPC);
}

TEST(Interface, APutMakesTheQueueFreeAgain) {
  fixture f;

  f.netdev->add_if(1, 3);
  auto interface = f.make(1);

  ASSERT_EQ(interface->get_queue(), 1);
  ASSERT_EQ(interface->get_queue(), 2);
  ASSERT_EQ(interface->get_queue(), -ENOSPC);

  EXPECT_EQ(interface->put_queue(1), 0);
  EXPECT_EQ(interface->get_queue(), 1);
}

TEST(Interface, PutRefusesAFreeOrUnknownQueue) {
  fixture f;

  f.netdev->add_if(1, 3);
  auto interface = f.make(1);

  EXPECT_EQ(interface->put_queue(2), -EINVAL);
  EXPECT_EQ(interface->put_queue(99), -EINVAL);
  /* Queue 0 belongs to the kernel, so no instance may give it back. */
  EXPECT_EQ(interface->put_queue(0), -EINVAL);
}

TEST(Interface, NoCombinedChannelHandsOutNoQueue) {
  fixture f;

  f.netdev->add_if(1, 0);

  /* queues[0] = true on an empty vector was a write past the end. */
  auto interface = f.make(1);
  EXPECT_EQ(interface->queue_count(), 0u);
  EXPECT_EQ(interface->get_queue(), -ENOSPC);
}

TEST(Interface, ConstructionClearsTheRulesTheDeviceHeld) {
  fixture f;

  f.netdev->add_if(1);
  f.netdev->ifaces[1].rules = {2, 5};

  auto interface = f.make(1);
  EXPECT_EQ(f.netdev->deleted.size(), 2u);
  EXPECT_TRUE(f.netdev->ifaces[1].rules.empty());
}

TEST(Interface, AddFlowInsertsAtTheFreeLocation) {
  fixture f;

  f.netdev->add_if(1, 4, 8);
  auto interface = f.make(1);

  int id = interface->add_flow(2, 0x02, 0x0100000a, 0x0200000a, 1234, 5678);
  ASSERT_EQ(id, 7);
  ASSERT_EQ(f.netdev->inserted.size(), 1u);
  EXPECT_EQ(f.netdev->inserted_at[0], 7u);

  /* The record must reach the device unchanged. A swap here would steer the
   * traffic to the wrong address. */
  EXPECT_EQ(f.netdev->inserted[0].flow_type, 0x02u);
  EXPECT_EQ(f.netdev->inserted[0].src_ip, 0x0100000au);
  EXPECT_EQ(f.netdev->inserted[0].dst_ip, 0x0200000au);
  EXPECT_EQ(f.netdev->inserted[0].src_port, 1234);
  EXPECT_EQ(f.netdev->inserted[0].dst_port, 5678);
  EXPECT_EQ(f.netdev->inserted[0].queue_id, 2);

  /* The next rule goes one slot up the table. */
  EXPECT_EQ(interface->add_flow(2, 0x02, 0, 0, 0, 5679), 6);
}

TEST(Interface, AddFlowReportsAFullTable) {
  fixture f;

  f.netdev->add_if(1, 4, 2);
  auto interface = f.make(1);

  ASSERT_EQ(interface->add_flow(1, 0x02, 0, 0, 0, 1000), 1);
  EXPECT_EQ(interface->add_flow(1, 0x02, 0, 0, 0, 1001), -ENOSPC);
  /* A refusal must not reach the device. */
  EXPECT_EQ(f.netdev->insert_calls, 1);
}

TEST(Interface, AddFlowPassesOnADeviceFailure) {
  fixture f;

  f.netdev->add_if(1);
  auto interface = f.make(1);

  f.netdev->insert_rule_ret = -EOPNOTSUPP;
  EXPECT_EQ(interface->add_flow(1, 0x02, 0, 0, 0, 1000), -EOPNOTSUPP);

  f.netdev->get_rules_ret = -EPERM;
  EXPECT_EQ(interface->add_flow(1, 0x02, 0, 0, 0, 1000), -EPERM);
}

TEST(Interface, DelFlowPassesOnADeviceFailure) {
  fixture f;

  f.netdev->add_if(1);
  auto interface = f.make(1);

  int id = interface->add_flow(1, 0x02, 0, 0, 0, 1000);
  ASSERT_GT(id, 0);
  EXPECT_EQ(interface->del_flow(static_cast<uint32_t>(id)), 0);
  /* The rule is gone, so a second delete cannot find it. */
  EXPECT_EQ(interface->del_flow(static_cast<uint32_t>(id)), -ENOENT);
}

TEST(Interface, XsksMapFdNeedsAnAttachedProgram) {
  fixture f;

  f.netdev->add_if(1);
  f.xdp->map_fd = 11;

  auto with_xdp = f.make(1);
  EXPECT_TRUE(with_xdp->has_xdp());
  EXPECT_EQ(with_xdp->get_xsks_map_fd(), 11);

  fixture g;
  g.netdev->add_if(1);
  g.xdp->attach_ret = -ENOTSUP;
  auto without_xdp = g.make(1);
  EXPECT_EQ(without_xdp->get_xsks_map_fd(), -1);
}

TEST(Interface, UdpFilterNeedsAnAttachedProgram) {
  fixture f;

  f.netdev->add_if(1);
  auto interface = f.make(1);

  EXPECT_EQ(interface->update_udp_dp_filter(20000, true), 0);
  ASSERT_EQ(f.xdp->filter_calls.size(), 1u);
  EXPECT_EQ(f.xdp->filter_calls[0].first, 20000);
  EXPECT_TRUE(f.xdp->filter_calls[0].second);

  fixture g;
  g.netdev->add_if(1);
  g.xdp->attach_ret = -ENOTSUP;
  auto without_xdp = g.make(1);
  EXPECT_EQ(without_xdp->update_udp_dp_filter(20000, true), -ENOTSUP);
  EXPECT_TRUE(g.xdp->filter_calls.empty());
}

TEST(Interface, DestructionDetachesAndClearsTheRules) {
  fixture f;

  f.netdev->add_if(1);
  {
    auto interface = f.make(1);
    ASSERT_GT(interface->add_flow(1, 0x02, 0, 0, 0, 1000), 0);
    ASSERT_EQ(f.netdev->ifaces[1].rules.size(), 1u);
  }

  EXPECT_EQ(f.xdp->detach_calls, 1);
  EXPECT_TRUE(f.netdev->ifaces[1].rules.empty());
}

TEST(InterfaceRegistry, OneObjectPerIndex) {
  auto netdev = std::make_shared<fake_netdev>();
  auto xdp = std::make_shared<fake_xdp_state>();

  netdev->add_if(1);
  netdev->add_if(2);
  mtl_interface_registry registry(netdev, fake_xdp_factory(xdp));

  auto first = registry.get(1, false);
  auto again = registry.get(1, false);
  ASSERT_NE(first, nullptr);
  EXPECT_EQ(first.get(), again.get());
  EXPECT_EQ(registry.live_count(), 1u);

  auto other = registry.get(2, false);
  ASSERT_NE(other, nullptr);
  EXPECT_NE(first.get(), other.get());
  EXPECT_EQ(registry.live_count(), 2u);
}

TEST(InterfaceRegistry, AnInterfaceGoesAwayWithItsLastUser) {
  auto netdev = std::make_shared<fake_netdev>();
  auto xdp = std::make_shared<fake_xdp_state>();

  netdev->add_if(1);
  mtl_interface_registry registry(netdev, fake_xdp_factory(xdp));

  {
    auto held = registry.get(1, false);
    ASSERT_NE(held, nullptr);
    EXPECT_EQ(registry.live_count(), 1u);
  }

  /* The queues and the rules must come back when the last client leaves. */
  EXPECT_EQ(registry.live_count(), 0u);
  EXPECT_EQ(xdp->detach_calls, 1);

  auto fresh = registry.get(1, false);
  ASSERT_NE(fresh, nullptr);
  EXPECT_EQ(xdp->attach_calls, 2);
}

TEST(InterfaceRegistry, AFailureIsAnEmptyPointerNotAnException) {
  auto netdev = std::make_shared<fake_netdev>();
  auto xdp = std::make_shared<fake_xdp_state>();

  mtl_interface_registry registry(netdev, fake_xdp_factory(xdp));

  /* No such interface. The server must keep serving. */
  EXPECT_EQ(registry.get(99, false), nullptr);
  EXPECT_EQ(registry.live_count(), 0u);
}
