/*!
 * ST 2110 RTP Session Common
 *
 * This module defines the RTP session structure and its associated methods
 * which is used for specific RTP sessions creation.
 *
 */

use derive_builder::Builder;
use std::net::Ipv4Addr;

#[derive(Clone, Builder, Debug)]
#[builder(setter(into))]
pub struct RtpSession {
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
    /// Retrieves the IP address used by this RTP session.
    pub fn ip(&self) -> Ipv4Addr {
        self.ip
    }

    /// Retrieves the udp dst port used by this RTP session.
    pub fn port(&self) -> u16 {
        self.port
    }

    /// Retrieves the payload type used by this RTP session.
    pub fn payload_type(&self) -> u8 {
        self.payload_type
    }

    /// Retrieves the RTCP option of this RTP session.
    pub fn enable_rtcp(&self) -> bool {
        self.enable_rtcp
    }

    /// Retrieves the synchronization source identifier (SSRC) used by this RTP session.
    pub fn ssrc(&self) -> Option<u32> {
        self.ssrc
    }

    /// Retrieves the multicast source IP address used by this RTP session.
    pub fn mcast_source_ip(&self) -> Option<Ipv4Addr> {
        self.mcast_source_ip
    }

    /// Retrieves the name of this RTP session.
    pub fn name(&self) -> Option<&str> {
        self.name.as_deref()
    }
}

/// Provides a default implementation for `RtpSession`, used when no initial values are
/// explicitly provided for the new RTP session instances.
impl Default for RtpSession {
    fn default() -> Self {
        Self {
            ip: Ipv4Addr::new(239, 0, 0, 1),
            port: 0,
            payload_type: 0,
            enable_rtcp: false,
            ssrc: None,
            mcast_source_ip: None,
            name: None,
        }
    }
}
