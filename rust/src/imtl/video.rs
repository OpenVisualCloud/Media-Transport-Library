/*!
 * ST 2110-20 and ST 2110-22 Video Session
 *
 * This module defines the structures and implementations necessary for setting up
 * RTP sessions for transmitting and receiving uncompressed (ST 2110-20) and
 * compressed video (ST 2110-22 / JPEG-XS).
 *
 */

use crate::mtl::Mtl;
use crate::session::RtpSession;
use crate::sys;
use anyhow::{bail, Result};
use crossbeam_utils::sync::Parker;
use derive_builder::Builder;
use std::{ffi::c_void, fmt::Display, mem::MaybeUninit, str::FromStr};

#[derive(Clone, Debug)]
enum VideoHandle {
    Tx(sys::st20_tx_handle),
    Rx(sys::st20_rx_handle),
    PipelineTx(sys::st20p_tx_handle),
    PipelineRx(sys::st20p_rx_handle),
    PipelineCompressedTx(sys::st22p_tx_handle),
    PipelineCompressedRx(sys::st22p_rx_handle),
}

impl Drop for VideoHandle {
    fn drop(&mut self) {
        match self {
            VideoHandle::Tx(h) => unsafe {
                sys::st20_tx_free(*h);
            },
            VideoHandle::Rx(h) => unsafe {
                sys::st20_rx_free(*h);
            },
            VideoHandle::PipelineTx(h) => unsafe {
                sys::st20p_tx_free(*h);
            },
            VideoHandle::PipelineRx(h) => unsafe {
                sys::st20p_rx_free(*h);
            },
            VideoHandle::PipelineCompressedTx(h) => unsafe {
                sys::st22p_tx_free(*h);
            },
            VideoHandle::PipelineCompressedRx(h) => unsafe {
                sys::st22p_rx_free(*h);
            },
        }
    }
}

/// Different packing formats for uncompressed video.
#[derive(Copy, Clone, Debug, Default)]
pub enum Packing {
    #[default]
    Bpm = sys::st20_packing_ST20_PACKING_BPM as _, // Block Packing Mode
    Gpm = sys::st20_packing_ST20_PACKING_GPM as _, // General Packing Mode
    GpmSl = sys::st20_packing_ST20_PACKING_GPM_SL as _, // General Packing Mode with Single Line
}

/// Different frame rates (frames per second) supported.
#[derive(Copy, Clone, Debug, Default)]
pub enum Fps {
    #[default]
    P59_94 = sys::st_fps_ST_FPS_P59_94 as _,
    P50 = sys::st_fps_ST_FPS_P50 as _,
    P29_97 = sys::st_fps_ST_FPS_P29_97 as _,
    P25 = sys::st_fps_ST_FPS_P25 as _,
    P119_88 = sys::st_fps_ST_FPS_P119_88 as _,
    P120 = sys::st_fps_ST_FPS_P120 as _,
    P100 = sys::st_fps_ST_FPS_P100 as _,
    P60 = sys::st_fps_ST_FPS_P60 as _,
    P30 = sys::st_fps_ST_FPS_P30 as _,
    P24 = sys::st_fps_ST_FPS_P24 as _,
    P23_98 = sys::st_fps_ST_FPS_P23_98 as _,
}

impl Display for Fps {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            Fps::P59_94 => write!(f, "59.94"),
            Fps::P50 => write!(f, "50"),
            Fps::P29_97 => write!(f, "29.97"),
            Fps::P25 => write!(f, "25"),
            Fps::P119_88 => write!(f, "119.88"),
            Fps::P120 => write!(f, "120"),
            Fps::P100 => write!(f, "100"),
            Fps::P60 => write!(f, "60"),
            Fps::P30 => write!(f, "30"),
            Fps::P24 => write!(f, "24"),
            Fps::P23_98 => write!(f, "23.98"),
        }
    }
}

impl FromStr for Fps {
    type Err = anyhow::Error;

    fn from_str(fps: &str) -> Result<Self> {
        match fps {
            "59.94" => Ok(Fps::P59_94),
            "50" => Ok(Fps::P50),
            "29.97" => Ok(Fps::P29_97),
            "25" => Ok(Fps::P25),
            "119.88" => Ok(Fps::P119_88),
            "120" => Ok(Fps::P120),
            "100" => Ok(Fps::P100),
            "60" => Ok(Fps::P60),
            "30" => Ok(Fps::P30),
            "24" => Ok(Fps::P24),
            "23.98" => Ok(Fps::P23_98),
            _ => bail!("Invalid FPS value"),
        }
    }
}

impl Fps {
    /// convert Fps to float
    pub fn to_float(self) -> f32 {
        match self {
            Fps::P59_94 => 60000.0 / 1001.0,
            Fps::P50 => 50000.0 / 1000.0,
            Fps::P29_97 => 30000.0 / 1001.0,
            Fps::P25 => 25000.0 / 1000.0,
            Fps::P119_88 => 11988.0 / 1000.0,
            Fps::P120 => 12000.0 / 1000.0,
            Fps::P100 => 10000.0 / 1000.0,
            Fps::P60 => 60000.0 / 1000.0,
            Fps::P30 => 30000.0 / 1000.0,
            Fps::P24 => 24000.0 / 1000.0,
            Fps::P23_98 => 24000.0 / 1001.0,
        }
    }

    pub fn duration(self, frame_count: u32) -> std::time::Duration {
        std::time::Duration::from_secs_f32(frame_count as f32 / self.to_float())
    }
}

/// Different transport formats for raw video data.
#[derive(Copy, Clone, Debug, Default)]
pub enum TransportFmt {
    #[default]
    Yuv422_8bit = sys::st20_fmt_ST20_FMT_YUV_422_8BIT as _,
    Yuv422_10bit = sys::st20_fmt_ST20_FMT_YUV_422_10BIT as _,
    Yuv422_12bit = sys::st20_fmt_ST20_FMT_YUV_422_12BIT as _,
    Yuv422_16bit = sys::st20_fmt_ST20_FMT_YUV_422_16BIT as _,
    Yuv420_8bit = sys::st20_fmt_ST20_FMT_YUV_420_8BIT as _,
    Yuv420_10bit = sys::st20_fmt_ST20_FMT_YUV_420_10BIT as _,
    Yuv420_12bit = sys::st20_fmt_ST20_FMT_YUV_420_12BIT as _,
    Yuv420_16bit = sys::st20_fmt_ST20_FMT_YUV_420_16BIT as _,
    Rgb8bit = sys::st20_fmt_ST20_FMT_RGB_8BIT as _,
    Rgb10bit = sys::st20_fmt_ST20_FMT_RGB_10BIT as _,
    Rgb12bit = sys::st20_fmt_ST20_FMT_RGB_12BIT as _,
    Rgb16bit = sys::st20_fmt_ST20_FMT_RGB_16BIT as _,
    Yuv444_8bit = sys::st20_fmt_ST20_FMT_YUV_444_8BIT as _,
    Yuv444_10bit = sys::st20_fmt_ST20_FMT_YUV_444_10BIT as _,
    Yuv444_12bit = sys::st20_fmt_ST20_FMT_YUV_444_12BIT as _,
    Yuv444_16bit = sys::st20_fmt_ST20_FMT_YUV_444_16BIT as _,
}

impl Display for TransportFmt {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            TransportFmt::Yuv422_8bit => write!(f, "yuv_422_8bit"),
            TransportFmt::Yuv422_10bit => write!(f, "yuv_422_10bit"),
            TransportFmt::Yuv422_12bit => write!(f, "yuv_422_12bit"),
            TransportFmt::Yuv422_16bit => write!(f, "yuv_422_16bit"),
            TransportFmt::Yuv420_8bit => write!(f, "yuv_420_8bit"),
            TransportFmt::Yuv420_10bit => write!(f, "yuv_420_10bit"),
            TransportFmt::Yuv420_12bit => write!(f, "yuv_420_12bit"),
            TransportFmt::Yuv420_16bit => write!(f, "yuv_420_16bit"),
            TransportFmt::Rgb8bit => write!(f, "rgb_8bit"),
            TransportFmt::Rgb10bit => write!(f, "rgb_10bit"),
            TransportFmt::Rgb12bit => write!(f, "rgb_12bit"),
            TransportFmt::Rgb16bit => write!(f, "rgb_16bit"),
            TransportFmt::Yuv444_8bit => write!(f, "yuv_444_8bit"),
            TransportFmt::Yuv444_10bit => write!(f, "yuv_444_10bit"),
            TransportFmt::Yuv444_12bit => write!(f, "yuv_444_12bit"),
            TransportFmt::Yuv444_16bit => write!(f, "yuv_444_16bit"),
        }
    }
}

impl FromStr for TransportFmt {
    type Err = anyhow::Error;

    fn from_str(s: &str) -> Result<Self> {
        match s {
            "yuv_422_8bit" => Ok(TransportFmt::Yuv422_8bit),
            "yuv_422_10bit" => Ok(TransportFmt::Yuv422_10bit),
            "yuv_422_12bit" => Ok(TransportFmt::Yuv422_12bit),
            "yuv_422_16bit" => Ok(TransportFmt::Yuv422_16bit),
            "yuv_420_8bit" => Ok(TransportFmt::Yuv420_8bit),
            "yuv_420_10bit" => Ok(TransportFmt::Yuv420_10bit),
            "yuv_420_12bit" => Ok(TransportFmt::Yuv420_12bit),
            "yuv_420_16bit" => Ok(TransportFmt::Yuv420_16bit),
            "rgb_8bit" => Ok(TransportFmt::Rgb8bit),
            "rgb_10bit" => Ok(TransportFmt::Rgb10bit),
            "rgb_12bit" => Ok(TransportFmt::Rgb12bit),
            "rgb_16bit" => Ok(TransportFmt::Rgb16bit),
            "yuv_444_8bit" => Ok(TransportFmt::Yuv444_8bit),
            "yuv_444_10bit" => Ok(TransportFmt::Yuv444_10bit),
            "yuv_444_12bit" => Ok(TransportFmt::Yuv444_12bit),
            "yuv_444_16bit" => Ok(TransportFmt::Yuv444_16bit),
            _ => bail!(format!("Unknown format: {}", s)),
        }
    }
}

/// States that a frame can be in during processing.
#[derive(Copy, Clone, Debug, Default, PartialEq)]
enum FrameStatus {
    #[default]
    Free,
    InUse,
    Ready,
}

#[derive(Copy, Clone, Debug, Default)]
pub enum FrameFmt {
    Yuv422Planar10Le = sys::st_frame_fmt_ST_FRAME_FMT_YUV422PLANAR10LE as _,
    V210 = sys::st_frame_fmt_ST_FRAME_FMT_V210 as _,
    Y210 = sys::st_frame_fmt_ST_FRAME_FMT_Y210 as _,
    Yuv422Planar8 = sys::st_frame_fmt_ST_FRAME_FMT_YUV422PLANAR8 as _,
    Uyvy = sys::st_frame_fmt_ST_FRAME_FMT_UYVY as _,
    #[default]
    Yuv422Rfc4175Pg2Be10 = sys::st_frame_fmt_ST_FRAME_FMT_YUV422RFC4175PG2BE10 as _,
    Yuv422Planar12Le = sys::st_frame_fmt_ST_FRAME_FMT_YUV422PLANAR12LE as _,
    Yuv422Rfc4175Pg2Be12 = sys::st_frame_fmt_ST_FRAME_FMT_YUV422RFC4175PG2BE12 as _,
    Yuv444Planar10Le = sys::st_frame_fmt_ST_FRAME_FMT_YUV444PLANAR10LE as _,
    Yuv444Rfc4175Pg4Be10 = sys::st_frame_fmt_ST_FRAME_FMT_YUV444RFC4175PG4BE10 as _,
    Yuv444Planar12Le = sys::st_frame_fmt_ST_FRAME_FMT_YUV444PLANAR12LE as _,
    Yuv444Rfc4175Pg2Be12 = sys::st_frame_fmt_ST_FRAME_FMT_YUV444RFC4175PG2BE12 as _,
    Yuv420Custom8 = sys::st_frame_fmt_ST_FRAME_FMT_YUV420CUSTOM8 as _,
    Yuv422Custom8 = sys::st_frame_fmt_ST_FRAME_FMT_YUV422CUSTOM8 as _,
    Argb = sys::st_frame_fmt_ST_FRAME_FMT_ARGB as _,
    Bgra = sys::st_frame_fmt_ST_FRAME_FMT_BGRA as _,
    Rgb8 = sys::st_frame_fmt_ST_FRAME_FMT_RGB8 as _,
    Gbrplanar10Le = sys::st_frame_fmt_ST_FRAME_FMT_GBRPLANAR10LE as _,
    RgbRfc4175Pg4Be10 = sys::st_frame_fmt_ST_FRAME_FMT_RGBRFC4175PG4BE10 as _,
    Gbrplanar12Le = sys::st_frame_fmt_ST_FRAME_FMT_GBRPLANAR12LE as _,
    RgbRfc4175Pg2Be12 = sys::st_frame_fmt_ST_FRAME_FMT_RGBRFC4175PG2BE12 as _,
    JpegxsCodestream = sys::st_frame_fmt_ST_FRAME_FMT_JPEGXS_CODESTREAM as _,
    H264CbrCodestream = sys::st_frame_fmt_ST_FRAME_FMT_H264_CBR_CODESTREAM as _,
    H264Codestream = sys::st_frame_fmt_ST_FRAME_FMT_H264_CODESTREAM as _,
    H265CbrCodestream = sys::st_frame_fmt_ST_FRAME_FMT_H265_CBR_CODESTREAM as _,
    H265Codestream = sys::st_frame_fmt_ST_FRAME_FMT_H265_CODESTREAM as _,
}

impl FromStr for FrameFmt {
    type Err = anyhow::Error;

    fn from_str(s: &str) -> Result<Self> {
        use FrameFmt::*;
        match s {
            "YUV422PLANAR10LE" => Ok(Yuv422Planar10Le),
            "V210" => Ok(V210),
            "Y210" => Ok(Y210),
            "YUV422PLANAR8" => Ok(Yuv422Planar8),
            "UYVY" => Ok(Uyvy),
            "YUV422RFC4175PG2BE10" => Ok(Yuv422Rfc4175Pg2Be10),
            "YUV422PLANAR12LE" => Ok(Yuv422Planar12Le),
            "YUV422RFC4175PG2BE12" => Ok(Yuv422Rfc4175Pg2Be12),
            "YUV444PLANAR10LE" => Ok(Yuv444Planar10Le),
            "YUV444RFC4175PG4BE10" => Ok(Yuv444Rfc4175Pg4Be10),
            "YUV444PLANAR12LE" => Ok(Yuv444Planar12Le),
            "YUV444RFC4175PG2BE12" => Ok(Yuv444Rfc4175Pg2Be12),
            "YUV420CUSTOM8" => Ok(Yuv420Custom8),
            "YUV422CUSTOM8" => Ok(Yuv422Custom8),
            "ARGB" => Ok(Argb),
            "BGRA" => Ok(Bgra),
            "RGB8" => Ok(Rgb8),
            "GBRPLANAR10LE" => Ok(Gbrplanar10Le),
            "RGBRFC4175PG4BE10" => Ok(RgbRfc4175Pg4Be10),
            "GBRPLANAR12LE" => Ok(Gbrplanar12Le),
            "RGBRFC4175PG2BE12" => Ok(RgbRfc4175Pg2Be12),
            "JPEGXS_CODESTREAM" => Ok(JpegxsCodestream),
            "H264_CBR_CODESTREAM" => Ok(H264CbrCodestream),
            _ => bail!(format!("Unknown format: {}", s)),
        }
    }
}

impl Display for FrameFmt {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            FrameFmt::Yuv422Planar10Le => write!(f, "YUV422PLANAR10LE"),
            FrameFmt::V210 => write!(f, "V210"),
            FrameFmt::Y210 => write!(f, "Y210"),
            FrameFmt::Yuv422Planar8 => write!(f, "YUV422PLANAR8"),
            FrameFmt::Uyvy => write!(f, "UYVY"),
            FrameFmt::Yuv422Rfc4175Pg2Be10 => write!(f, "YUV422RFC4175PG2BE10"),
            FrameFmt::Yuv422Planar12Le => write!(f, "YUV422PLANAR12LE"),
            FrameFmt::Yuv422Rfc4175Pg2Be12 => write!(f, "YUV422RFC4175PG2BE12"),
            FrameFmt::Yuv444Planar10Le => write!(f, "YUV444PLANAR10LE"),
            FrameFmt::Yuv444Rfc4175Pg4Be10 => write!(f, "YUV444RFC4175PG4BE10"),
            FrameFmt::Yuv444Planar12Le => write!(f, "YUV444PLANAR12LE"),
            FrameFmt::Yuv444Rfc4175Pg2Be12 => write!(f, "YUV444RFC4175PG2BE12"),
            FrameFmt::Yuv420Custom8 => write!(f, "YUV420CUSTOM8"),
            FrameFmt::Yuv422Custom8 => write!(f, "YUV422CUSTOM8"),
            FrameFmt::Argb => write!(f, "ARGB"),
            FrameFmt::Bgra => write!(f, "BGRA"),
            FrameFmt::Rgb8 => write!(f, "RGB8"),
            FrameFmt::Gbrplanar10Le => write!(f, "GBRPLANAR10LE"),
            FrameFmt::RgbRfc4175Pg4Be10 => write!(f, "RGBRFC4175PG4BE10"),
            FrameFmt::Gbrplanar12Le => write!(f, "GBRPLANAR12LE"),
            FrameFmt::RgbRfc4175Pg2Be12 => write!(f, "RGBRFC4175PG2BE12"),
            FrameFmt::JpegxsCodestream => write!(f, "JPEGXS_CODESTREAM"),
            FrameFmt::H264CbrCodestream => write!(f, "H264_CBR_CODESTREAM"),
        }
    }
}

impl FrameFmt {
    pub fn frame_size(&self, width: u32, height: u32) -> Result<usize> {
        use FrameFmt::*;
        match self {
            Yuv422Planar10Le | Y210 | Yuv422Planar12Le => Ok(width as usize * height as usize * 4),
            V210 => Ok(width as usize * height as usize * 16 / 6),
            Yuv422Planar8 | Uyvy | Yuv422Custom8 => Ok(width as usize * height as usize * 2),
            Yuv422Rfc4175Pg2Be10 => Ok(width as usize * height as usize * 5 / 2),
            Yuv422Rfc4175Pg2Be12 => Ok(width as usize * height as usize * 3),
            Yuv444Planar10Le | Yuv444Planar12Le | Gbrplanar10Le | Gbrplanar12Le => {
                Ok(width as usize * height as usize * 6)
            }
            Yuv444Rfc4175Pg4Be10 | RgbRfc4175Pg4Be10 => {
                Ok(width as usize * height as usize * 15 / 4)
            }
            Yuv444Rfc4175Pg2Be12 | RgbRfc4175Pg2Be12 => {
                Ok(width as usize * height as usize * 9 / 2)
            }
            Yuv420Custom8 => Ok(width as usize * height as usize * 6 / 4),
            Argb | Bgra => Ok(width as usize * height as usize * 4),
            Rgb8 => Ok(width as usize * height as usize * 3),
            _ => bail!("Unknown frame size"),
        }
    }
}

/// VideoTx structure for handling transmission of uncompressed video.
#[derive(Default, Builder, Debug)]
#[builder(setter(into))]
pub struct VideoTx {
    #[builder(default)]
    netdev_id: u8,
    #[builder(default)]
    rtp_session: RtpSession,
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

    // For pipeline API
    #[builder(default)]
    input_fmt: Option<FrameFmt>,

    #[builder(setter(skip))]
    handle: Option<VideoHandle>,
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

unsafe extern "C" fn video_tx_get_next_frame(
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

unsafe extern "C" fn video_tx_notify_frame_done(
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

unsafe extern "C" fn st20p_tx_notify_frame_done(p_void: *mut c_void, _: *mut sys::st_frame) -> i32 {
    unsafe {
        let s: &mut VideoTx = &mut *(p_void as *mut VideoTx);
        let u = s.parker.unparker().clone();
        u.unpark();
        0
    }
}

impl VideoTx {
    /// Initializes a new VideoTx session with Media Transport Library (MTL) handle.
    pub fn create(mut self, mtl: &Mtl) -> Result<Self> {
        if self.handle.is_some() {
            bail!("VideoTx Session is already created");
        }

        self.parker = Parker::new();

        if let Some(input_fmt) = self.input_fmt {
            // Use pipeline API

            let mut ops: MaybeUninit<sys::st20p_tx_ops> = MaybeUninit::uninit();

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
                ops.transport_packing = self.packing as _;
                ops.width = self.width as _;
                ops.height = self.height as _;
                ops.fps = self.fps as _;
                ops.input_fmt = input_fmt as _;
                ops.transport_fmt = self.t_fmt as _;
                ops.interlaced = self.interlaced as _;
                ops.framebuff_cnt = self.fb_cnt as _;
                ops.device = sys::st_plugin_device_ST_PLUGIN_DEVICE_AUTO; // only set auto for now

                let pointer_to_void: *mut c_void = &self as *const VideoTx as *mut c_void;
                ops.priv_ = pointer_to_void;
                ops.notify_frame_done = Some(st20p_tx_notify_frame_done);
            }

            let mut ops = unsafe { ops.assume_init() };

            let handle = unsafe { sys::st20p_tx_create(mtl.handle().unwrap(), &mut ops as *mut _) };
            if handle.is_null() {
                bail!("Failed to initialize MTL")
            } else {
                self.handle = Some(VideoHandle::PipelineTx(handle));
                Ok(self)
            }
        } else {
            for _ in 0..self.fb_cnt {
                self.frame_status.push(FrameStatus::Free);
            }
            self.consumer_idx = 0;
            self.producer_idx = 0;

            let mut ops: MaybeUninit<sys::st20_tx_ops> = MaybeUninit::uninit();

            // Fill the ops
            unsafe {
                std::ptr::write_bytes(ops.as_mut_ptr(), 0, 1);
                let ops = &mut *ops.as_mut_ptr();
                ops.num_port = 1;
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
                ops.get_next_frame = Some(video_tx_get_next_frame);
                ops.notify_frame_done = Some(video_tx_notify_frame_done);
            }

            let mut ops = unsafe { ops.assume_init() };

            let handle = unsafe { sys::st20_tx_create(mtl.handle().unwrap(), &mut ops as *mut _) };
            if handle.is_null() {
                bail!("Failed to initialize MTL VideoTx")
            } else {
                self.handle = Some(VideoHandle::Tx(handle));
                unsafe {
                    self.frame_size = sys::st20_tx_get_framebuffer_size(handle);
                }
                Ok(self)
            }
        }
    }

    /// Get the raw frame size
    pub fn frame_size(&self) -> usize {
        self.frame_size
    }

    /// Wait until free frame available, default timeout is 1 frame interval
    pub fn wait_free_frame(&mut self) {
        match self.handle {
            Some(VideoHandle::Tx(_)) => {
                let frame_idx = self.producer_idx;
                if !self.is_frame_free(frame_idx) {
                    self.parker.park_timeout(self.fps.duration(1));
                }
            }
            Some(VideoHandle::PipelineTx(_)) => self.parker.park_timeout(self.fps.duration(1)),
            _ => (),
        }
    }

    /// Fill the frame buffer to be transmitted
    pub fn fill_next_frame(&mut self, frame: &[u8]) -> Result<()> {
        match self.handle {
            Some(VideoHandle::Tx(handle)) => {
                let mut frame_idx = self.producer_idx;
                while !self.is_frame_free(frame_idx) {
                    frame_idx = self.next_frame_idx(frame_idx);
                    if frame_idx == self.producer_idx {
                        bail!("No free frames");
                    }
                }

                unsafe {
                    let frame_dst = sys::st20_tx_get_framebuffer(handle, frame_idx as _);
                    // memcpy frame to frame_dst with size
                    sys::mtl_memcpy(frame_dst, frame.as_ptr() as _, self.frame_size);
                }

                self.set_frame_ready(frame_idx);
                self.producer_idx = self.next_frame_idx(frame_idx);
                Ok(())
            }
            Some(VideoHandle::PipelineTx(handle)) => {
                unsafe {
                    let inner_frame = sys::st20p_tx_get_frame(handle);
                    if inner_frame.is_null() {
                        bail!("No free frames");
                    }
                    // memcpy frame to frame_dst with size, assume lines no padding
                    sys::mtl_memcpy(
                        (*inner_frame).addr[0],
                        frame.as_ptr() as _,
                        (*inner_frame).data_size,
                    );
                    sys::st20p_tx_put_frame(handle, inner_frame);
                }
                Ok(())
            }
            _ => bail!("Invalid handle"),
        }
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

/// VideoRx structure for handling receiving of uncompressed video.
#[derive(Default, Builder, Debug)]
#[builder(setter(into))]
pub struct VideoRx {
    #[builder(default)]
    netdev_id: u8,
    #[builder(default)]
    rtp_session: RtpSession,
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

    // For pipeline API
    #[builder(default)]
    output_fmt: Option<FrameFmt>,

    #[builder(setter(skip))]
    handle: Option<VideoHandle>,
    #[builder(setter(skip))]
    consumer_idx: u8,
    #[builder(setter(skip))]
    producer_idx: u8,
    #[builder(setter(skip))]
    frame_size: usize,
    #[builder(setter(skip))]
    parker: Parker,
    #[builder(setter(skip))]
    frames: Vec<*mut c_void>,
}

unsafe extern "C" fn video_rx_notify_frame_ready(
    p_void: *mut c_void,
    framebuffer: *mut c_void,
    _: *mut sys::st20_rx_frame_meta,
) -> i32 {
    unsafe {
        let s: &mut VideoRx = &mut *(p_void as *mut VideoRx);
        match s.enqueue_frame(framebuffer) {
            Ok(_) => {
                let u = s.parker.unparker().clone();
                u.unpark();
                0
            }
            Err(_) => -16, // return -EBUSY
        }
    }
}

unsafe extern "C" fn st20p_rx_notify_frame_available(p_void: *mut c_void) -> i32 {
    unsafe {
        let s: &mut VideoRx = &mut *(p_void as *mut VideoRx);
        let u = s.parker.unparker().clone();
        u.unpark();
        0
    }
}

impl VideoRx {
    /// Initializes a new VideoRx session with Media Transport Library (MTL) handle and Netdev ID.
    pub fn create(mut self, mtl: &Mtl) -> Result<Self> {
        if self.handle.is_some() {
            bail!("VideoRx Session is already created");
        }

        self.parker = Parker::new();

        if let Some(output_fmt) = self.output_fmt {
            let mut ops: MaybeUninit<sys::st20p_rx_ops> = MaybeUninit::uninit();

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
                ops.width = self.width as _;
                ops.height = self.height as _;
                ops.fps = self.fps as _;
                ops.output_fmt = output_fmt as _;
                ops.transport_fmt = self.t_fmt as _;
                ops.interlaced = self.interlaced as _;
                ops.framebuff_cnt = self.fb_cnt as _;
                ops.device = sys::st_plugin_device_ST_PLUGIN_DEVICE_AUTO; // only set auto for now

                let pointer_to_void: *mut c_void = &self as *const VideoRx as *mut c_void;
                ops.priv_ = pointer_to_void;
                ops.notify_frame_available = Some(st20p_rx_notify_frame_available);
            }

            let mut ops = unsafe { ops.assume_init() };

            let handle = unsafe { sys::st20p_rx_create(mtl.handle().unwrap(), &mut ops as *mut _) };
            if handle.is_null() {
                bail!("Failed to initialize MTL")
            } else {
                self.handle = Some(VideoHandle::PipelineRx(handle));
                Ok(self)
            }
        } else {
            for _ in 0..self.fb_cnt {
                self.frames.push(std::ptr::null_mut());
            }

            self.consumer_idx = 0;
            self.producer_idx = 0;

            let mut ops: MaybeUninit<sys::st20_rx_ops> = MaybeUninit::uninit();

            // Fill the ops
            unsafe {
                std::ptr::write_bytes(ops.as_mut_ptr(), 0, 1);
                let ops = &mut *ops.as_mut_ptr();
                ops.num_port = 1;
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
                ops.port[0].copy_from_slice(&port_bytes);
                ops.__bindgen_anon_1.ip_addr[0] = self.rtp_session.ip().octets();
                ops.udp_port[0] = self.rtp_session.port() as _;
                ops.payload_type = self.rtp_session.payload_type() as _;
                ops.width = self.width as _;
                ops.height = self.height as _;
                ops.fps = self.fps as _;
                ops.fmt = self.t_fmt as _;
                ops.interlaced = self.interlaced as _;
                ops.framebuff_cnt = self.fb_cnt as _;

                let pointer_to_void: *mut c_void = &self as *const VideoRx as *mut c_void;
                ops.priv_ = pointer_to_void;
                ops.notify_frame_ready = Some(video_rx_notify_frame_ready);
            }

            let mut ops = unsafe { ops.assume_init() };

            let handle = unsafe { sys::st20_rx_create(mtl.handle().unwrap(), &mut ops as *mut _) };
            if handle.is_null() {
                bail!("Failed to initialize MTL VideoRx")
            } else {
                self.handle = Some(VideoHandle::Rx(handle));
                unsafe {
                    self.frame_size = sys::st20_rx_get_framebuffer_size(handle);
                }
                Ok(self)
            }
        }
    }

    /// Get the raw frame size
    pub fn frame_size(&self) -> usize {
        self.frame_size
    }

    /// Wait until new frame available, default timeout is 1 frame interval
    pub fn wait_new_frame(&mut self) {
        match self.handle {
            Some(VideoHandle::Rx(_)) => {
                if self.frames[self.consumer_idx as usize].is_null() {
                    self.parker.park_timeout(self.fps.duration(1));
                }
            }
            Some(VideoHandle::PipelineRx(_)) => {
                self.parker.park_timeout(self.fps.duration(1));
            }
            _ => (),
        }
    }

    /// Copy the new frame to user provided memory
    pub fn fill_new_frame(&mut self, data: &[u8]) -> Result<()> {
        match self.handle {
            Some(VideoHandle::Rx(handle)) => {
                let frame = self.frames[self.consumer_idx as usize];
                if frame.is_null() {
                    bail!("No frame available");
                } else {
                    unsafe {
                        sys::mtl_memcpy(data.as_ptr() as _, frame as _, self.frame_size);
                        sys::st20_rx_put_framebuff(handle, frame as _);
                    }
                    self.frames[self.consumer_idx as usize] = std::ptr::null_mut();
                    self.consumer_idx = self.next_frame_idx(self.consumer_idx);
                    Ok(())
                }
            }
            Some(VideoHandle::PipelineRx(handle)) => {
                unsafe {
                    let frame = sys::st20p_rx_get_frame(handle);
                    if frame.is_null() {
                        bail!("No frame available");
                    }
                    // assume lines no padding
                    sys::mtl_memcpy(data.as_ptr() as _, (*frame).addr[0], (*frame).data_size);
                    sys::st20p_rx_put_frame(handle, frame);
                }
                Ok(())
            }
            _ => bail!("Invalid handle"),
        }
    }

    fn enqueue_frame(&mut self, frame: *mut c_void) -> Result<()> {
        let producer_idx = self.producer_idx;

        if !self.frames[producer_idx as usize].is_null() {
            bail!("Frame is busy");
        }

        self.frames[producer_idx as usize] = frame;
        self.producer_idx = self.next_frame_idx(producer_idx);
        Ok(())
    }

    fn next_frame_idx(&self, frame_idx: u8) -> u8 {
        (frame_idx + 1) % self.fb_cnt
    }
}

/// CompressedVideoTx structure for handling transmission of compressed video.
#[derive(Default, Builder, Debug)]
#[builder(setter(into))]
pub struct CompressedVideoTx {
    #[builder(default)]
    netdev_id: u8,
    #[builder(default)]
    rtp_session: RtpSession,
    #[builder(default = "1920")]
    width: u32,
    #[builder(default = "1080")]
    height: u32,
    #[builder(default)]
    fps: Fps,
    #[builder(default = "3")]
    fb_cnt: u8,
    #[builder(default = "false")]
    interlaced: bool,
    #[builder(default)]
    input_fmt: FrameFmt,
    #[builder(default = "2")]
    codec_thread_cnt: u8,
    #[builder(default = "3")]
    bpp: u8,

    #[builder(setter(skip))]
    handle: Option<VideoHandle>,
}

impl CompressedVideoTx {
    /// Initializes a new CompressedVideoTx session with Media Transport Library (MTL) handle.
    pub fn create(mut self, mtl: &Mtl) -> Result<Self> {
        if self.handle.is_some() {
            bail!("CompressedVideoTx Session is already created");
        }

        let mut ops: MaybeUninit<sys::st22p_tx_ops> = MaybeUninit::uninit();

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
            ops.width = self.width as _;
            ops.height = self.height as _;
            ops.fps = self.fps as _;
            ops.input_fmt = self.input_fmt as _;
            ops.interlaced = self.interlaced as _;
            ops.pack_type = sys::st22_pack_type_ST22_PACK_CODESTREAM;
            ops.codec = sys::st22_codec_ST22_CODEC_JPEGXS;
            ops.device = sys::st_plugin_device_ST_PLUGIN_DEVICE_AUTO;
            ops.quality = sys::st22_quality_mode_ST22_QUALITY_MODE_QUALITY;
            ops.codec_thread_cnt = self.codec_thread_cnt as _;
            ops.codestream_size =
                self.width as usize * self.height as usize * self.bpp as usize / 8;
            if ops.interlaced {
                ops.codestream_size /= 2;
            }
            ops.framebuff_cnt = self.fb_cnt as _;
            ops.flags = sys::st22p_tx_flag_ST22P_TX_FLAG_BLOCK_GET;

            let pointer_to_void: *mut c_void = &self as *const CompressedVideoTx as *mut c_void;
            ops.priv_ = pointer_to_void;
        }

        let mut ops = unsafe { ops.assume_init() };

        let handle = unsafe { sys::st22p_tx_create(mtl.handle().unwrap(), &mut ops as *mut _) };
        if handle.is_null() {
            bail!("Failed to initialize MTL CompressedVideoTx")
        } else {
            self.handle = Some(VideoHandle::PipelineCompressedTx(handle));
            Ok(self)
        }
    }

    /// Fill the frame buffer to be transmitted
    pub fn fill_next_frame(&mut self, frame: &[u8]) -> Result<()> {
        match self.handle {
            Some(VideoHandle::PipelineCompressedTx(handle)) => {
                unsafe {
                    let inner_frame = sys::st22p_tx_get_frame(handle);
                    if inner_frame.is_null() {
                        bail!("Time-out get frame");
                    }
                    // memcpy frame to frame_dst with size, assume lines no padding
                    sys::mtl_memcpy(
                        (*inner_frame).addr[0],
                        frame.as_ptr() as _,
                        (*inner_frame).data_size,
                    );
                    sys::st22p_tx_put_frame(handle, inner_frame);
                }
                Ok(())
            }
            _ => bail!("Invalid handle"),
        }
    }
}

/// CompressedVideoRx structure for handling receiving of compressed video.
#[derive(Default, Builder, Debug)]
#[builder(setter(into))]
pub struct CompressedVideoRx {
    #[builder(default)]
    netdev_id: u8,
    #[builder(default)]
    rtp_session: RtpSession,
    #[builder(default = "1920")]
    width: u32,
    #[builder(default = "1080")]
    height: u32,
    #[builder(default)]
    fps: Fps,
    #[builder(default = "3")]
    fb_cnt: u8,
    #[builder(default = "false")]
    interlaced: bool,
    #[builder(default)]
    output_fmt: FrameFmt,
    #[builder(default = "2")]
    codec_thread_cnt: u8,

    #[builder(setter(skip))]
    handle: Option<VideoHandle>,
}

impl CompressedVideoRx {
    /// Initializes a new CompressedVideoRx session with Media Transport Library (MTL) handle.
    pub fn create(mut self, mtl: &Mtl) -> Result<Self> {
        if self.handle.is_some() {
            bail!("CompressedVideoRx Session is already created");
        }

        let mut ops: MaybeUninit<sys::st22p_rx_ops> = MaybeUninit::uninit();
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
            ops.width = self.width as _;
            ops.height = self.height as _;
            ops.fps = self.fps as _;
            ops.output_fmt = self.output_fmt as _;
            ops.interlaced = self.interlaced as _;
            ops.framebuff_cnt = self.fb_cnt as _;
            ops.device = sys::st_plugin_device_ST_PLUGIN_DEVICE_AUTO;
            ops.pack_type = sys::st22_pack_type_ST22_PACK_CODESTREAM;
            ops.codec = sys::st22_codec_ST22_CODEC_JPEGXS;
            ops.max_codestream_size = 0; /* let lib to decide */
            ops.codec_thread_cnt = self.codec_thread_cnt as _;
            ops.flags = sys::st22p_rx_flag_ST22P_RX_FLAG_BLOCK_GET;

            let pointer_to_void: *mut c_void = &self as *const CompressedVideoRx as *mut c_void;
            ops.priv_ = pointer_to_void;
        }
        let mut ops = unsafe { ops.assume_init() };

        let handle = unsafe { sys::st22p_rx_create(mtl.handle().unwrap(), &mut ops as *mut _) };
        if handle.is_null() {
            bail!("Failed to initialize MTL CompressedVideoRx")
        } else {
            self.handle = Some(VideoHandle::PipelineCompressedRx(handle));
            Ok(self)
        }
    }

    /// Copy the new frame to user provided memory
    pub fn fill_new_frame(&mut self, data: &[u8]) -> Result<()> {
        match self.handle {
            Some(VideoHandle::PipelineCompressedRx(handle)) => {
                unsafe {
                    let frame = sys::st22p_rx_get_frame(handle);
                    if frame.is_null() {
                        bail!("Time-out get frame");
                    }
                    // assume lines no padding
                    sys::mtl_memcpy(data.as_ptr() as _, (*frame).addr[0], (*frame).data_size);
                    sys::st22p_rx_put_frame(handle, frame);
                }
                Ok(())
            }
            _ => bail!("Invalid handle"),
        }
    }
}
