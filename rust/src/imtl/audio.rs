/*!
 * ST 2110-30/31 Audio Session
 *
 * This module defines the structures and implementations necessary for setting up
 * RTP sessions for transmitting and receiving uncompressed (ST 2110-30 / AES67) and
 * compressed audio (ST 2110-31 / AES3).
 *
 */

use crate::mtl::Mtl;
use crate::session::RtpSession;
use crate::sys;
use anyhow::{bail, Result};
use derive_builder::Builder;
use std::{ffi::c_void, fmt::Display, mem::MaybeUninit, str::FromStr};

#[derive(Clone, Debug)]
enum AudioHandle {
    Tx(sys::st30p_tx_handle),
    Rx(sys::st30p_rx_handle),
}

impl Drop for AudioHandle {
    fn drop(&mut self) {
        match self {
            AudioHandle::Tx(h) => unsafe {
                sys::st30p_tx_free(*h);
            },
            AudioHandle::Rx(h) => unsafe {
                sys::st30p_rx_free(*h);
            },
        }
    }
}

/// Payload format of st2110-30/31(audio) streaming.
#[derive(Copy, Clone, Debug, Default)]
pub enum Fmt {
    #[default]
    Pcm8 = sys::st30_fmt_ST30_FMT_PCM8 as _,
    Pcm16 = sys::st30_fmt_ST30_FMT_PCM16 as _,
    Pcm24 = sys::st30_fmt_ST30_FMT_PCM24 as _,
    Am824 = sys::st30_fmt_ST31_FMT_AM824 as _,
}

impl Display for Fmt {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            Fmt::Pcm8 => write!(f, "PCM 8bit"),
            Fmt::Pcm16 => write!(f, "PCM 16bit"),
            Fmt::Pcm24 => write!(f, "PCM 24bit"),
            Fmt::Am824 => write!(f, "AM824 32bit"),
        }
    }
}

impl FromStr for Fmt {
    type Err = anyhow::Error;

    fn from_str(s: &str) -> Result<Self, Self::Err> {
        match s {
            "pcm8" => Ok(Fmt::Pcm8),
            "pcm16" => Ok(Fmt::Pcm16),
            "pcm24" => Ok(Fmt::Pcm24),
            "am824" => Ok(Fmt::Am824),
            _ => bail!("Unknown format: {}", s),
        }
    }
}

/// Sampling rate of st2110-30/31(audio) streaming.
#[derive(Copy, Clone, Debug, Default)]
pub enum Sampling {
    #[default]
    S48K = sys::st30_sampling_ST30_SAMPLING_48K as _,
    S96K = sys::st30_sampling_ST30_SAMPLING_96K as _,
    S44K = sys::st30_sampling_ST31_SAMPLING_44K as _,
}

impl Display for Sampling {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            Sampling::S48K => write!(f, "48kHz"),
            Sampling::S96K => write!(f, "96kHz"),
            Sampling::S44K => write!(f, "44.1kHz"),
        }
    }
}

impl FromStr for Sampling {
    type Err = anyhow::Error;

    fn from_str(s: &str) -> Result<Self, Self::Err> {
        match s {
            "48k" => Ok(Sampling::S48K),
            "96k" => Ok(Sampling::S96K),
            "44.1k" => Ok(Sampling::S44K),
            _ => bail!("Unknown sampling rate: {}", s),
        }
    }
}

/// Packet time period of st2110-30/31(audio) streaming.
#[derive(Copy, Clone, Debug, Default)]
pub enum PacketTime {
    #[default]
    P1ms = sys::st30_ptime_ST30_PTIME_1MS as _,
    P125us = sys::st30_ptime_ST30_PTIME_125US as _,
    P250us = sys::st30_ptime_ST30_PTIME_250US as _,
    P333us = sys::st30_ptime_ST30_PTIME_333US as _,
    P4ms = sys::st30_ptime_ST30_PTIME_4MS as _,
    P80us = sys::st30_ptime_ST31_PTIME_80US as _,
    P1_09ms = sys::st30_ptime_ST31_PTIME_1_09MS as _,
    P0_14ms = sys::st30_ptime_ST31_PTIME_0_14MS as _,
    P0_09ms = sys::st30_ptime_ST31_PTIME_0_09MS as _,
}

impl Display for PacketTime {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            PacketTime::P1ms => write!(f, "1ms"),
            PacketTime::P125us => write!(f, "125us"),
            PacketTime::P250us => write!(f, "250us"),
            PacketTime::P333us => write!(f, "333us"),
            PacketTime::P4ms => write!(f, "4ms"),
            PacketTime::P80us => write!(f, "80us"),
            PacketTime::P1_09ms => write!(f, "1.09ms"),
            PacketTime::P0_14ms => write!(f, "0.14ms"),
            PacketTime::P0_09ms => write!(f, "0.09ms"),
        }
    }
}

impl FromStr for PacketTime {
    type Err = anyhow::Error;

    fn from_str(s: &str) -> Result<Self, Self::Err> {
        match s {
            "1ms" => Ok(PacketTime::P1ms),
            "125us" => Ok(PacketTime::P125us),
            "250us" => Ok(PacketTime::P250us),
            "333us" => Ok(PacketTime::P333us),
            "4ms" => Ok(PacketTime::P4ms),
            "80us" => Ok(PacketTime::P80us),
            "1.09ms" => Ok(PacketTime::P1_09ms),
            "0.14ms" => Ok(PacketTime::P0_14ms),
            "0.09ms" => Ok(PacketTime::P0_09ms),
            _ => bail!("Unknown packet time: {}", s),
        }
    }
}

/// AudioTx structure for handling transmission of audio.
#[derive(Default, Builder, Debug)]
#[builder(setter(into))]
pub struct AudioTx {
    #[builder(default)]
    netdev_id: u8,
    #[builder(default)]
    rtp_session: RtpSession,
    #[builder(default)]
    fmt: Fmt,
    #[builder(default = "2")]
    channel: u16,
    #[builder(default)]
    sampling: Sampling,
    #[builder(default)]
    ptime: PacketTime,
    #[builder(default = "3")]
    fb_cnt: u8,
    #[builder(default = "10")]
    frametime_ms: u16,

    #[builder(setter(skip))]
    handle: Option<AudioHandle>,
    #[builder(setter(skip))]
    frame_size: usize,
}

impl AudioTx {
    /// Initializes a new AudioTx session with Media Transport Library (MTL) handle.
    pub fn create(mut self, mtl: &Mtl) -> Result<Self> {
        if self.handle.is_some() {
            bail!("VideoTx Session is already created");
        }

        let mut ops: MaybeUninit<sys::st30p_tx_ops> = MaybeUninit::uninit();

        // Fill the ops
        unsafe {
            std::ptr::write_bytes(ops.as_mut_ptr(), 0, 1);
            let ops = &mut *ops.as_mut_ptr();
            ops.port.num_port = 1;
            ops.name = self.rtp_session.name().unwrap().as_ptr() as *const i8;

            let id = self.netdev_id as usize;
            let net_dev = mtl.net_devs().get(id).unwrap();
            let port_bytes: Vec<i8> = net_dev
                .get_port()
                .as_bytes()
                .iter()
                .cloned()
                .map(|b| b as i8) // Convert u8 to i8
                .chain(std::iter::repeat(0)) // Pad with zeros if needed
                .take(64) // Take only up to 64 elements
                .collect();
            ops.port.port[0].copy_from_slice(&port_bytes);
            ops.port.dip_addr[0] = self.rtp_session.ip().octets();
            ops.port.udp_port[0] = self.rtp_session.port() as _;
            ops.port.payload_type = self.rtp_session.payload_type() as _;
            ops.fmt = self.fmt as _;
            ops.channel = self.channel as _;
            ops.sampling = self.sampling as _;
            ops.ptime = self.ptime as _;
            ops.framebuff_size = sys::st30_calculate_framebuff_size(
                ops.fmt,
                ops.ptime,
                ops.sampling,
                ops.channel,
                self.frametime_ms as u64 * 1000000,
                0 as _,
            ) as _;
            ops.framebuff_cnt = self.fb_cnt as _;
            ops.flags = sys::st30p_tx_flag_ST30P_TX_FLAG_BLOCK_GET;

            let pointer_to_void: *mut c_void = &self as *const AudioTx as *mut c_void;
            ops.priv_ = pointer_to_void;
        }

        let mut ops = unsafe { ops.assume_init() };

        let handle = unsafe { sys::st30p_tx_create(mtl.handle().unwrap(), &mut ops as *mut _) };
        if handle.is_null() {
            bail!("Failed to initialize MTL AudioTx")
        } else {
            self.handle = Some(AudioHandle::Tx(handle));
            unsafe {
                self.frame_size = sys::st30p_tx_frame_size(handle);
            }
            Ok(self)
        }
    }

    /// Get the raw frame size
    pub fn frame_size(&self) -> usize {
        self.frame_size
    }

    /// Fill the frame buffer to be transmitted
    pub fn fill_next_frame(&mut self, frame: &[u8]) -> Result<()> {
        match self.handle {
            Some(AudioHandle::Tx(handle)) => {
                let frame_size = self.frame_size;
                if frame.len() != frame_size {
                    bail!(
                        "Frame size mismatch: expected {}, got {}",
                        frame_size,
                        frame.len()
                    );
                }
                unsafe {
                    let inner_frame = sys::st30p_tx_get_frame(handle);
                    if inner_frame.is_null() {
                        bail!("Time-out get frame");
                    }

                    // memcpy frame to frame_dst with size, assume lines no padding
                    sys::mtl_memcpy(
                        (*inner_frame).addr,
                        frame.as_ptr() as _,
                        (*inner_frame).data_size,
                    );
                    sys::st30p_tx_put_frame(handle, inner_frame);
                }
                Ok(())
            }
            _ => bail!("Invalid handle"),
        }
    }
}

/// AudioRx structure for handling receiving of audio.
#[derive(Default, Builder, Debug)]
#[builder(setter(into))]
pub struct AudioRx {
    #[builder(default)]
    netdev_id: u8,
    #[builder(default)]
    rtp_session: RtpSession,
    #[builder(default)]
    fmt: Fmt,
    #[builder(default = "2")]
    channel: u16,
    #[builder(default)]
    sampling: Sampling,
    #[builder(default)]
    ptime: PacketTime,
    #[builder(default = "3")]
    fb_cnt: u8,
    #[builder(default = "10")]
    frametime_ms: u16,

    #[builder(setter(skip))]
    handle: Option<AudioHandle>,
    #[builder(setter(skip))]
    frame_size: usize,
}

impl AudioRx {
    /// Initializes a new AudioRx session with Media Transport Library (MTL) handle.
    pub fn create(mut self, mtl: &Mtl) -> Result<Self> {
        if self.handle.is_some() {
            bail!("VideoTx Session is already created");
        }

        let mut ops: MaybeUninit<sys::st30p_rx_ops> = MaybeUninit::uninit();

        // Fill the ops
        unsafe {
            std::ptr::write_bytes(ops.as_mut_ptr(), 0, 1);
            let ops = &mut *ops.as_mut_ptr();
            ops.port.num_port = 1;
            ops.name = self.rtp_session.name().unwrap().as_ptr() as *const i8;

            let id = self.netdev_id as usize;
            let net_dev = mtl.net_devs().get(id).unwrap();
            let port_bytes: Vec<i8> = net_dev
                .get_port()
                .as_bytes()
                .iter()
                .cloned()
                .map(|b| b as i8) // Convert u8 to i8
                .chain(std::iter::repeat(0)) // Pad with zeros if needed
                .take(64) // Take only up to 64 elements
                .collect();
            ops.port.port[0].copy_from_slice(&port_bytes);
            ops.port.__bindgen_anon_1.ip_addr[0] = self.rtp_session.ip().octets();
            ops.port.udp_port[0] = self.rtp_session.port() as _;
            ops.port.payload_type = self.rtp_session.payload_type() as _;
            ops.fmt = self.fmt as _;
            ops.channel = self.channel as _;
            ops.sampling = self.sampling as _;
            ops.ptime = self.ptime as _;
            ops.framebuff_size = sys::st30_calculate_framebuff_size(
                ops.fmt,
                ops.ptime,
                ops.sampling,
                ops.channel,
                self.frametime_ms as u64 * 1000000,
                0 as _,
            ) as _;
            ops.framebuff_cnt = self.fb_cnt as _;
            ops.flags = sys::st30p_rx_flag_ST30P_RX_FLAG_BLOCK_GET;

            let pointer_to_void: *mut c_void = &self as *const AudioRx as *mut c_void;
            ops.priv_ = pointer_to_void;
        }

        let mut ops = unsafe { ops.assume_init() };

        let handle = unsafe { sys::st30p_rx_create(mtl.handle().unwrap(), &mut ops as *mut _) };
        if handle.is_null() {
            bail!("Failed to initialize MTL AudioRx")
        } else {
            self.handle = Some(AudioHandle::Rx(handle));
            unsafe {
                self.frame_size = sys::st30p_rx_frame_size(handle);
            }
            Ok(self)
        }
    }

    /// Get the raw frame size
    pub fn frame_size(&self) -> usize {
        self.frame_size
    }

    /// Copy the new frame to user provided memory
    pub fn fill_new_frame(&mut self, data: &[u8]) -> Result<()> {
        match self.handle {
            Some(AudioHandle::Rx(handle)) => {
                let frame_size = self.frame_size;
                if data.len() != frame_size {
                    bail!(
                        "Frame size mismatch: expected {}, got {}",
                        frame_size,
                        data.len()
                    );
                }
                unsafe {
                    let frame = sys::st30p_rx_get_frame(handle);
                    if frame.is_null() {
                        bail!("Time-out get frame");
                    }
                    // assume lines no padding
                    sys::mtl_memcpy(data.as_ptr() as _, (*frame).addr, (*frame).data_size);
                    sys::st30p_rx_put_frame(handle, frame);
                }
                Ok(())
            }
            _ => bail!("Invalid handle"),
        }
    }
}
