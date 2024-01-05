/*!
ST 2110 RTP Session Common
 */

use derive_builder::Builder;

use crate::netdev::NetDev;
use std::{net::Ipv4Addr, rc::Rc};

#[derive(Clone, Builder, Debug)]
#[builder(setter(into))]
pub struct RtpSession {
    net_dev: Rc<NetDev>,
    ip: Ipv4Addr,
    port: u16,
    payload_type: u8,
    name: Option<String>,
    #[builder(default = "false")]
    enable_rtcp: bool,
    #[builder(default)]
    ssrc: Option<u32>,
    #[builder(default)]
    mcast_source_ip: Option<Ipv4Addr>,
}

impl RtpSession {
    pub fn net_dev(&self) -> &NetDev {
        &self.net_dev
    }

    pub fn ip(&self) -> Ipv4Addr {
        self.ip
    }

    pub fn port(&self) -> u16 {
        self.port
    }

    pub fn payload_type(&self) -> u8 {
        self.payload_type
    }

    pub fn enable_rtcp(&self) -> bool {
        self.enable_rtcp
    }

    pub fn ssrc(&self) -> Option<u32> {
        self.ssrc
    }

    pub fn mcast_source_ip(&self) -> Option<Ipv4Addr> {
        self.mcast_source_ip
    }

    pub fn name(&self) -> Option<&str> {
        self.name.as_deref()
    }
}

impl Default for RtpSession {
    fn default() -> Self {
        Self {
            ip: Ipv4Addr::new(127, 0, 0, 1),
            net_dev: Rc::new(NetDev::default()),
            port: 0,
            payload_type: 0,
            enable_rtcp: false,
            ssrc: None,
            mcast_source_ip: None,
            name: None,
        }
    }
}
