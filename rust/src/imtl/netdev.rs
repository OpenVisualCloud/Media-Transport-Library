use std::net::Ipv4Addr;

#[derive(Copy, Clone)]
pub enum MtlNetProto {
    Static = 0,
    Dhcp,
}

#[derive(Copy, Clone)]
pub enum MtlPmdType {
    DpdkPmd = 0,
    DpdkAfXdp,
    DpdkAfPacket,
    KernelSocket,
    AfXdp,
}

pub struct MtlNetDev {
    port: String,
    net_proto: MtlNetProto,
    pmd: MtlPmdType,
    sip_addr: Option<Ipv4Addr>,
    netmask: Option<Ipv4Addr>,
    gateway: Option<Ipv4Addr>,
    tx_queues_cnt: u16,
    rx_queues_cnt: u16,
    xdp_start_queue: u8,
}

impl MtlNetDev {
    pub fn new(
        port: String,
        net_proto: MtlNetProto,
        pmd: MtlPmdType,
        sip_addr: Option<Ipv4Addr>,
        netmask: Option<Ipv4Addr>,
        gateway: Option<Ipv4Addr>,
        tx_queues_cnt: u16,
        rx_queues_cnt: u16,
        xdp_start_queue: u8,
    ) -> MtlNetDev {
        MtlNetDev {
            port: port,
            net_proto: net_proto,
            pmd: pmd,
            sip_addr: sip_addr,
            netmask: netmask,
            gateway: gateway,
            tx_queues_cnt: tx_queues_cnt,
            rx_queues_cnt: rx_queues_cnt,
            xdp_start_queue: xdp_start_queue,
        }
    }

    pub fn get_port(&self) -> &str {
        &self.port
    }

    pub fn get_net_proto(&self) -> MtlNetProto {
        self.net_proto
    }

    pub fn get_pmd(&self) -> MtlPmdType {
        self.pmd
    }

    pub fn get_sip_addr(&self) -> Option<Ipv4Addr> {
        self.sip_addr
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
