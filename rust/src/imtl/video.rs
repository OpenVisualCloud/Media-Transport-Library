/*!
 * ST 2110-20 and ST 2110-22 Video Session
 *
 * This module defines the structures and implementations necessary for setting up
 * RTP sessions for transmitting and receiving uncompressed (ST 2110-20) and
 * compressed video (ST 2110-22).
 *
 */

use crate::mtl::Mtl;
use crate::session::RtpSession;
use crate::sys;
use anyhow::{bail, Result};
use crossbeam_utils::sync::Parker;
use derive_builder::Builder;
use std::{ffi::c_void, mem::MaybeUninit};

/// Different packing formats for uncompressed video.
#[derive(Copy, Clone, Debug, Default)]
pub enum Packing {
    #[default]
    Bpm = 0, // Block Packing Mode
    Gpm,   // General Packing Mode
    GpmSl, // General Packing Mode with Single Line
}

/// Different frame rates (frames per second) supported.
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

/// Different transport formats for raw video data.
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

/// States that a frame can be in during processing.
#[derive(Copy, Clone, Debug, Default, PartialEq)]
enum FrameStatus {
    #[default]
    Free,
    InUse,
    Ready,
}

/// VideoTx structure for handling transmission of uncompressed video.
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

    #[builder(setter(skip))]
    consumer_idx: u8,
    #[builder(setter(skip))]
    producer_idx: u8,
    #[builder(setter(skip))]
    frame_status: Vec<FrameStatus>,
    #[builder(setter(skip))]
    frame_size: usize,
    #[builder(setter(skip))]
    parker: Parker,
}

unsafe extern "C" fn get_next_frame(
    p_void: *mut c_void,
    next_frame_idx: *mut u16,
    _: *mut sys::st20_tx_frame_meta,
) -> i32 {
    unsafe {
        let s: &mut VideoTx = &mut *(p_void as *mut VideoTx);
        let nfi = s.consumer_idx;

        if s.is_frame_ready(nfi) {
            s.set_frame_in_use(nfi);
            *next_frame_idx = nfi as _;
            s.consumer_idx = s.next_frame_idx(nfi);
            0
        } else {
            -5 // invalid frame status, return -EIO
        }
    }
}

unsafe extern "C" fn notify_frame_done(
    p_void: *mut c_void,
    frame_idx: u16,
    _: *mut sys::st20_tx_frame_meta,
) -> i32 {
    unsafe {
        let s: &mut VideoTx = &mut *(p_void as *mut VideoTx);
        if s.is_frame_in_use(frame_idx as _) {
            s.set_frame_free(frame_idx as _);
            let u = s.parker.unparker().clone();
            u.unpark();
            0
        } else {
            -5 // invalid frame status, return -EIO
        }
    }
}

impl VideoTx {
    /// Initializes a new VideoTx session using the specified Media Transport Library (MTL).
    pub fn create(mut self, mtl: &Mtl) -> Result<Self> {
        if self.handle.is_some() {
            bail!("VideoTx Session is already created");
        }

        for _ in 0..self.fb_cnt {
            self.frame_status.push(FrameStatus::Free);
        }
        self.parker = Parker::new();
        self.consumer_idx = 0;
        self.producer_idx = 0;

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

            let pointer_to_void: *mut c_void = &self as *const VideoTx as *mut c_void;
            ops.priv_ = pointer_to_void;
            ops.get_next_frame = Some(get_next_frame);
            ops.notify_frame_done = Some(notify_frame_done);
        }

        let mut ops = unsafe { ops.assume_init() };

        let handle = unsafe { sys::st20_tx_create(mtl.handle().unwrap(), &mut ops as *mut _) };
        if handle == std::ptr::null_mut() {
            bail!("Failed to initialize MTL")
        } else {
            self.handle = Some(handle);
            unsafe {
                self.frame_size = sys::st20_tx_get_framebuffer_size(handle);
            }
            Ok(self)
        }
    }

    /// Get the raw frame size
    pub fn frame_size(&self) -> usize {
        self.frame_size
    }

    /// Wait until free frame available
    pub fn wait_free_frame(&mut self) {
        let frame_idx = self.producer_idx;
        if !self.is_frame_free(frame_idx) {
            self.parker.park();
        }
    }

    /// Fill the frame buffer to be transmitted
    pub fn fill_next_frame(&mut self, frame: &[u8]) -> Result<()> {
        let mut frame_idx = self.producer_idx;
        while !self.is_frame_free(frame_idx) {
            frame_idx = self.next_frame_idx(frame_idx);
            if frame_idx == self.producer_idx {
                bail!("No free frames");
            }
        }

        unsafe {
            let frame_dst = sys::st20_tx_get_framebuffer(self.handle.unwrap(), frame_idx as _);
            // memcpy frame to frame_dst with size
            sys::mtl_memcpy(frame_dst, frame.as_ptr() as _, self.frame_size);
        }

        self.set_frame_ready(frame_idx);
        self.producer_idx = self.next_frame_idx(frame_idx);
        Ok(())
    }

    fn set_frame_in_use(&mut self, frame_idx: u8) {
        self.frame_status[frame_idx as usize] = FrameStatus::InUse;
    }

    fn set_frame_free(&mut self, frame_idx: u8) {
        self.frame_status[frame_idx as usize] = FrameStatus::Free;
    }

    fn set_frame_ready(&mut self, frame_idx: u8) {
        self.frame_status[frame_idx as usize] = FrameStatus::Ready;
    }

    fn is_frame_in_use(&self, frame_idx: u8) -> bool {
        self.frame_status[frame_idx as usize] == FrameStatus::InUse
    }

    fn is_frame_free(&self, frame_idx: u8) -> bool {
        self.frame_status[frame_idx as usize] == FrameStatus::Free
    }

    fn is_frame_ready(&self, frame_idx: u8) -> bool {
        self.frame_status[frame_idx as usize] == FrameStatus::Ready
    }

    fn next_frame_idx(&self, frame_idx: u8) -> u8 {
        (frame_idx + 1) % self.fb_cnt
    }
}

impl Drop for VideoTx {
    /// Automatically called when the VideoTx instance is dropped from memory.
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
