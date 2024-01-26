use anyhow::{bail, Context, Result};
use clap::Parser;
use sdl2::pixels::PixelFormatEnum;
use sdl2::render::{Canvas, Texture};
use sdl2::video::Window;
use std::net::Ipv4Addr;
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::Arc;

use imtl::mtl::{Flags, LogLevel, MtlBuilder};
use imtl::netdev::*;
use imtl::session::RtpSessionBuilder;
use imtl::video::{Fps, FrameFmt, TransportFmt, VideoTxBuilder};

/// Simple program to use IMTL to send raw YUV frame from file
#[derive(Parser, Debug)]
#[command(author, version, about, long_about = None)]
struct Args {
    /// Name of the netdev
    #[arg(long, default_value_t = String::from("0000:4b:01.0"))]
    netdev: String,

    /// Netdev IP address
    #[arg(long, default_value_t = Ipv4Addr::new(192, 168, 96, 111))]
    sip: Ipv4Addr,

    /// Destination IP address
    #[arg(long, default_value_t = Ipv4Addr::new(239, 19, 96, 111))]
    ip: Ipv4Addr,

    /// Destination UDP Port number
    #[arg(long, default_value_t = 20000)]
    port: u16,

    /// Width
    #[arg(long, default_value_t = 1920)]
    width: u32,

    /// Height
    #[arg(long, default_value_t = 1080)]
    height: u32,

    /// Framerate
    #[arg(long, default_value_t = Fps::P60)]
    fps: Fps,

    /// Pipeline input format
    #[arg(long)]
    input_format: Option<FrameFmt>,

    /// Transport format
    #[arg(long, default_value_t = TransportFmt::Yuv422_10bit)]
    format: TransportFmt,

    /// Name of the YUV file
    #[arg(long)]
    yuv: String,

    /// Enable display window, only for 'yuv_422_8bit' format
    #[arg(long, default_value_t = false)]
    display: bool,

    /// Log level
    #[arg(short, long, default_value_t = LogLevel::Info)]
    log_level: LogLevel,
}

fn main() -> Result<()> {
    let args = Args::parse();

    let running = Arc::new(AtomicBool::new(true));
    let r = running.clone();
    ctrlc::set_handler(move || {
        r.store(false, Ordering::SeqCst);
    })
    .expect("Error setting Ctrl-C handler");

    /* open a yuv file from disk and map to memory */
    let yuv_file = std::fs::File::open(args.yuv)?;
    let yuv_file = unsafe { memmap2::MmapOptions::new().map(&yuv_file)? };

    let mut flags = Flags::empty();
    flags.insert(Flags::MTL_FLAG_BIND_NUMA | Flags::MTL_FLAG_DEV_AUTO_START_STOP);

    let net_dev0 = NetDevBuilder::default()
        .port(args.netdev)
        .pmd(PmdType::DpdkPmd)
        .net_proto(NetProto::Static)
        .ip(args.sip)
        .netmask("255.255.255.0".parse().ok())
        .gateway("0.0.0.0".parse().ok())
        .tx_queues_cnt(1u16)
        .rx_queues_cnt(0u16)
        .build()
        .context("Failed to add net dev")?;

    let net_devs = vec![net_dev0];

    let mtl = MtlBuilder::default()
        .net_devs(net_devs)
        .flags(flags)
        .log_level(args.log_level)
        .build()
        .unwrap()
        .init()
        .context("Failed to init mtl")?;

    let session = RtpSessionBuilder::default()
        .ip(args.ip)
        .port(args.port)
        .payload_type(112u8)
        .name(String::from("My Rust Video Tx"))
        .build()
        .context("Failed to add rtp session")?;

    let mut video_tx = VideoTxBuilder::default()
        .netdev_id(0)
        .rtp_session(session)
        .width(args.width)
        .height(args.height)
        .fps(args.fps)
        .input_fmt(args.input_format)
        .t_fmt(args.format)
        .build()
        .unwrap()
        .create(&mtl)
        .context("Failed to create tx video session")?;

    let sdl_context;
    let video_subsystem;
    let window;
    let texture_creator;
    let mut canvas: Option<Canvas<Window>> = None;
    let mut texture: Option<Texture> = None;
    if args.display {
        sdl_context = sdl2::init().unwrap();
        video_subsystem = sdl_context.video().unwrap();
        window = video_subsystem
            .window("IMTL TX Video", args.width / 4, args.height / 4)
            .position_centered()
            .opengl()
            .build()?;

        canvas = Some(window.into_canvas().build()?);
        texture_creator = canvas.as_ref().unwrap().texture_creator();
        texture = Some(texture_creator.create_texture_streaming(
            PixelFormatEnum::UYVY,
            args.width,
            args.height,
        )?);
    }

    let frame_size = if let Some(input_format) = args.input_format {
        input_format.frame_size(args.width, args.height)?
    } else {
        video_tx.frame_size()
    };
    let frames = yuv_file.chunks_exact(frame_size);
    if frames.len() == 0 {
        bail!("No frames in file");
    }
    let mut frames = frames.cycle();
    let mut frame = frames.next().unwrap();

    while running.load(Ordering::SeqCst) {
        match video_tx.fill_next_frame(frame) {
            Ok(_) => {
                if let (Some(ref mut texture), Some(ref mut canvas)) = (&mut texture, &mut canvas) {
                    texture.update(None, frame, args.width as usize * 2)?;
                    canvas.clear();
                    canvas.copy(texture, None, None).unwrap();
                    canvas.present();
                }
                frame = frames.next().unwrap();
            }
            Err(_) => {
                video_tx.wait_free_frame();
            }
        }
    }

    Ok(())
}
