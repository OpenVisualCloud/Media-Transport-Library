use std::net::Ipv4Addr;

use derive_builder::Builder;

#[derive(Copy, Clone, Debug, Default)]
pub enum NetProto {
    #[default]
    Static = 0,
    Dhcp,
}

#[derive(Copy, Clone, Debug, Default)]
pub enum PmdType {
    #[default]
    DpdkPmd = 0,
    DpdkAfXdp,
    DpdkAfPacket,
    KernelSocket,
    AfXdp,
}

#[derive(Clone, Default, Builder, Debug)]
#[builder(setter(into))]
pub struct NetDev {
    port: String,
    net_proto: NetProto,
    pmd: PmdType,
    #[builder(default)]
    ip: Option<Ipv4Addr>,
    #[builder(default)]
    netmask: Option<Ipv4Addr>,
    #[builder(default)]
    gateway: Option<Ipv4Addr>,
    tx_queues_cnt: u16,
    rx_queues_cnt: u16,
    #[builder(default)]
    xdp_start_queue: u8,
}

impl NetDev {
    pub fn get_port(&self) -> &str {
        &self.port
    }

    pub fn get_net_proto(&self) -> NetProto {
        self.net_proto
    }

    pub fn get_pmd(&self) -> PmdType {
        self.pmd
    }

    pub fn get_ip(&self) -> Option<Ipv4Addr> {
        self.ip
    }

    pub fn get_netmask(&self) -> Option<Ipv4Addr> {
        self.netmask
    }
    pub fn get_gateway(&self) -> Option<Ipv4Addr> {
        self.gateway
    }

    pub fn get_tx_queues_cnt(&self) -> u16 {
        self.tx_queues_cnt
    }

    pub fn get_rx_queues_cnt(&self) -> u16 {
        self.rx_queues_cnt
    }

    pub fn get_xdp_start_queue(&self) -> u8 {
        self.xdp_start_queue
    }

    pub fn bind_pmd(&self) -> Result<(), String> {
        // TODO
        Ok(())
    }

    pub fn bind_kernel(&self) -> Result<(), String> {
        // TODO
        Ok(())
    }
}
