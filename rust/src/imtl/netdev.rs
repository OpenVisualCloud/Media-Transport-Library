/*!
 * MTL Netdev
 *
 * This module provides definitions for configuring network devices in the system.
 */

use derive_builder::Builder;
use std::net::Ipv4Addr;

/// Enumeration representing the network protocol used by the network device.
#[derive(Copy, Clone, Debug, Default)]
pub enum NetProto {
    /// Default networking protocol, using static IP configuration.
    #[default]
    Static = 0,
    /// Dynamic Host Configuration Protocol.
    Dhcp,
}

/// Enumeration representing the type of Poll Mode Driver (PMD) used for packet processing.
#[derive(Copy, Clone, Debug, Default)]
pub enum PmdType {
    /// Default PMD type, using Data Plane Development Kit (DPDK).
    #[default]
    DpdkPmd = 0,
    /// DPDK with AF_XDP PMD.
    DpdkAfXdp,
    /// DPDK with AF_PACKET PMD.
    DpdkAfPacket,
    /// Kernel-based socket for standard packet processing.
    KernelSocket,
    /// AF_XDP (express data path) for high-performance packet processing without DPDK.
    AfXdp,
}

/// The `NetDev` struct defines the configuration for a network device.
#[derive(Clone, Default, Builder, Debug)]
#[builder(setter(into))]
pub struct NetDev {
    /// The network port identifier.
    port: String,
    /// The network protocol in use by the device.
    net_proto: NetProto,
    /// The type of PMD used for the device.
    pmd: PmdType,
    /// Optional IP address assigned to the network device.
    #[builder(default)]
    ip: Option<Ipv4Addr>,
    /// Optional network mask for the IP address.
    #[builder(default)]
    netmask: Option<Ipv4Addr>,
    /// Optional gateway IP address for the network device.
    #[builder(default)]
    gateway: Option<Ipv4Addr>,
    /// The number of transmit queues for the device.
    tx_queues_cnt: u16,
    /// The number of receive queues for the device.
    rx_queues_cnt: u16,
}

/// The `impl` block provides getter methods for `NetDev` properties, allowing
/// access to the configuration of the network device.
impl NetDev {
    /// Returns the port identifier.
    pub fn get_port(&self) -> &str {
        &self.port
    }

    /// Returns the network protocol used by the device.
    pub fn get_net_proto(&self) -> NetProto {
        self.net_proto
    }

    /// Returns the PMD type used by the device.
    pub fn get_pmd(&self) -> PmdType {
        self.pmd
    }

    /// Returns the IP address of the device, if set.
    pub fn get_ip(&self) -> Option<Ipv4Addr> {
        self.ip
    }

    /// Returns the network mask of the device, if set.
    pub fn get_netmask(&self) -> Option<Ipv4Addr> {
        self.netmask
    }

    /// Returns the gateway IP address of the device, if set.
    pub fn get_gateway(&self) -> Option<Ipv4Addr> {
        self.gateway
    }

    /// Returns the number of transmit queues for the device.
    pub fn get_tx_queues_cnt(&self) -> u16 {
        self.tx_queues_cnt
    }

    /// Returns the number of receive queues for the device.
    pub fn get_rx_queues_cnt(&self) -> u16 {
        self.rx_queues_cnt
    }
}
