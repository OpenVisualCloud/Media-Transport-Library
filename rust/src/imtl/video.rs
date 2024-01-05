/*!
ST 2110-20 and ST 2110-22 Video Session
 */

use crate::mtl::Mtl;
use crate::session::RtpSession;
use crate::sys;
use anyhow::{bail, Result};
use derive_builder::Builder;
use std::{ffi::c_void, mem::MaybeUninit};

#[derive(Copy, Clone, Debug, Default)]
pub enum Packing {
    #[default]
    Bpm = 0,
    Gpm,
    GpmSl,
}

#[derive(Copy, Clone, Debug, Default)]
pub enum Fps {
    #[default]
    P59_94 = 0,
    P50,
    P29_97,
    P25,
    P119_88,
    P120,
    P100,
    P60,
    P30,
    P24,
    P23_98,
}

#[derive(Copy, Clone, Debug, Default)]
pub enum TransportFmt {
    #[default]
    Yuv422_10bit = 0,
    Yuv422_8bit,
    Yuv422_12bit,
    Yuv422_16bit,
    Yuv420_8bit,
    Yuv420_10bit,
    Yuv420_12bit,
    Rgb8bit,
    Rgb10bit,
    Rgb12bit,
    Rgb16bit,
    Yuv444_8bit,
    Yuv444_10bit,
    Yuv444_12bit,
    Yuv444_16bit,
}

#[derive(Default, Builder, Debug)]
#[builder(setter(into))]
pub struct VideoTx {
    #[builder(default)]
    rtp_session: RtpSession,
    #[builder(default)]
    handle: Option<sys::st20_tx_handle>,
    #[builder(default = "1920")]
    width: u32,
    #[builder(default = "1080")]
    height: u32,
    #[builder(default)]
    fps: Fps,
    #[builder(default)]
    t_fmt: TransportFmt,
    #[builder(default)]
    packing: Packing,
    #[builder(default = "3")]
    fb_cnt: u8,
    #[builder(default = "false")]
    interlaced: bool,
    #[builder(default)]
    frame_in_use: u8,
    #[builder(default)]
    consumer_idx: u16,
}

unsafe extern "C" fn get_next_frame_dummy(
    p_void: *mut c_void,
    next_frame_idx: *mut u16,
    _: *mut sys::st20_tx_frame_meta,
) -> i32 {
    unsafe {
        let s: &mut VideoTx = &mut *(p_void as *mut VideoTx);
        let mut nfi = s.consumer_idx;
        nfi = (nfi + 1) % s.fb_cnt as u16;

        if s.is_frame_in_use(nfi) {
            return -1;
        }

        s.set_frame_in_use(nfi);
        s.consumer_idx = nfi;
        *next_frame_idx = nfi;
        0
    }
}

unsafe extern "C" fn notify_frame_done_dummy(
    p_void: *mut c_void,
    frame_idx: u16,
    _: *mut sys::st20_tx_frame_meta,
) -> i32 {
    unsafe {
        let s: &mut VideoTx = &mut *(p_void as *mut VideoTx);
        s.unset_frame_in_use(frame_idx);
        0
    }
}

impl VideoTx {
    pub fn create(mut self, mtl: &Mtl) -> Result<Self> {
        if self.handle.is_some() {
            bail!("VideoTx Session is already created");
        }

        let mut ops: MaybeUninit<sys::st20_tx_ops> = MaybeUninit::uninit();

        // Fill the ops
        unsafe {
            std::ptr::write_bytes(ops.as_mut_ptr(), 0, 1);
            let ops = &mut *ops.as_mut_ptr();
            ops.num_port = 1;
            ops.name = self.rtp_session.name().unwrap().as_ptr() as *const i8;

            let net_dev = self.rtp_session.net_dev();
            let port_bytes: Vec<i8> = net_dev
                .get_port()
                .as_bytes()
                .iter()
                .cloned()
                .map(|b| b as i8) // Convert u8 to i8
                .chain(std::iter::repeat(0)) // Pad with zeros if needed
                .take(64) // Take only up to 64 elements
                .collect();
            ops.port[0].copy_from_slice(&port_bytes);
            ops.dip_addr[0] = self.rtp_session.ip().octets();
            ops.udp_port[0] = self.rtp_session.port() as _;
            ops.payload_type = self.rtp_session.payload_type() as _;
            ops.packing = self.packing as _;
            ops.width = self.width as _;
            ops.height = self.height as _;
            ops.fps = self.fps as _;
            ops.fmt = self.t_fmt as _;
            ops.interlaced = self.interlaced as _;
            ops.framebuff_cnt = self.fb_cnt as _;

            // TODO add real callback functions
            let pointer_to_void: *mut c_void = &self as *const VideoTx as *mut c_void;
            ops.priv_ = pointer_to_void;
            ops.get_next_frame = Some(get_next_frame_dummy);
            ops.notify_frame_done = Some(notify_frame_done_dummy);
        }

        let mut ops = unsafe { ops.assume_init() };

        let handle = unsafe { sys::st20_tx_create(mtl.handle().unwrap(), &mut ops as *mut _) };
        if handle == std::ptr::null_mut() {
            bail!("Failed to initialize MTL")
        } else {
            self.handle = Some(handle);
            Ok(self)
        }
    }

    fn set_frame_in_use(&mut self, frame_idx: u16) {
        self.frame_in_use |= 1 << frame_idx;
    }

    fn unset_frame_in_use(&mut self, frame_idx: u16) {
        self.frame_in_use &= !(1 << frame_idx);
    }

    fn is_frame_in_use(&self, frame_idx: u16) -> bool {
        (self.frame_in_use & (1 << frame_idx)) != 0
    }
}

impl Drop for VideoTx {
    fn drop(&mut self) {
        if let Some(handle) = self.handle {
            unsafe {
                sys::st20_tx_free(handle);
            }
        }
    }
}

/* TODO
#[derive(Default, Builder, Debug)]
#[builder(setter(into))]
pub struct CompressedVideoTx {
    #[builder(default)]
    rtp_session: RtpSession,
    #[builder(default)]
    handle: Option<sys::st22_tx_handle>,
    #[builder(default = "1920")]
    width: u32,
    #[builder(default = "1080")]
    height: u32,
    #[builder(default)]
    fps: Fps,
    #[builder(default = "3")]
    fb_cnt: u8,
    fb_max_size: u32,
    #[builder(default = "false")]
    interlaced: bool,
}

#[derive(Default, Builder, Debug)]
#[builder(setter(into))]
pub struct VideoRx {
    #[builder(default)]
    rtp_session: RtpSession,
    #[builder(default)]
    handle: Option<sys::st20_rx_handle>,
    #[builder(default = "1920")]
    width: u32,
    #[builder(default = "1080")]
    height: u32,
    #[builder(default)]
    fps: Fps,
    #[builder(default)]
    t_fmt: TransportFmt,
    #[builder(default = "3")]
    fb_cnt: u8,
    #[builder(default = "false")]
    interlaced: bool,
}

#[derive(Default, Builder, Debug)]
#[builder(setter(into))]
pub struct CompressedVideoRx {
    #[builder(default)]
    rtp_session: RtpSession,
    #[builder(default)]
    handle: Option<sys::st22_rx_handle>,
    #[builder(default = "1920")]
    width: u32,
    #[builder(default = "1080")]
    height: u32,
    #[builder(default)]
    fps: Fps,
    #[builder(default = "3")]
    fb_cnt: u8,
    fb_max_size: u32,
    #[builder(default = "false")]
    interlaced: bool,
}



impl Drop for CompressedVideoTx {
    fn drop(&mut self) {
        if let Some(handle) = self.handle {
            unsafe {
                sys::st22_tx_free(handle);
            }
        }
    }
}

impl Drop for VideoRx {
    fn drop(&mut self) {
        if let Some(handle) = self.handle {
            unsafe {
                sys::st20_rx_free(handle);
            }
        }
    }
}

impl Drop for CompressedVideoRx {
    fn drop(&mut self) {
        if let Some(handle) = self.handle {
            unsafe {
                sys::st22_rx_free(handle);
            }
        }
    }
}
*/
