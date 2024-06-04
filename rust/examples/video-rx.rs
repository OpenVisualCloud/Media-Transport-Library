use anyhow::{bail, Context, Result};
use clap::Parser;
use sdl2::pixels::PixelFormatEnum;
use sdl2::render::{Canvas, Texture};
use sdl2::video::Window;
use std::io::Write;
use std::net::Ipv4Addr;
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::Arc;

use imtl::mtl::{Flags, LogLevel, MtlBuilder};
use imtl::netdev::*;
use imtl::session::RtpSessionBuilder;
use imtl::video::{Fps, FrameFmt, TransportFmt, VideoRxBuilder};

/// Simple program to use MTL to receive raw YUV frame and save the latest one to file
#[derive(Parser, Debug)]
#[command(author, version, about, long_about = None)]
struct Args {
    /// Name of the netdev
    #[arg(long, default_value_t = String::from("0000:4b:01.1"))]
    netdev: String,

    /// Netdev IP address
    #[arg(long, default_value_t = Ipv4Addr::new(192, 168, 96, 112))]
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

    /// Pipeline output format
    #[arg(long)]
    output_format: Option<FrameFmt>,

    /// Transport format
    #[arg(long, default_value_t = TransportFmt::Yuv422_10bit)]
    format: TransportFmt,

    /// Name of the YUV file
    #[arg(long)]
    yuv: Option<String>,

    /// Enable display window, only support for 'UYVY' output/transport format
    #[arg(long, default_value_t = false)]
    display: bool,

    /// Log level
    #[arg(short, long, default_value_t = LogLevel::Info)]
    log_level: LogLevel,
}

fn main() -> Result<()> {
    let args = Args::parse();
    let display = args.display;
    let save_yuv = args.yuv.is_some();
    if (save_yuv && display) || (!save_yuv && !display) {
        bail!("Only one of --yuv or --display should be set");
    }

    let running = Arc::new(AtomicBool::new(true));
    let r = running.clone();
    ctrlc::set_handler(move || {
        r.store(false, Ordering::SeqCst);
    })
    .expect("Error setting Ctrl-C handler");

    let mut flags = Flags::empty();
    flags.insert(Flags::MTL_FLAG_BIND_NUMA | Flags::MTL_FLAG_DEV_AUTO_START_STOP);

    let net_dev0 = NetDevBuilder::default()
        .port(args.netdev)
        .pmd(PmdType::DpdkPmd)
        .net_proto(NetProto::Static)
        .ip(args.sip)
        .netmask("255.255.255.0".parse().ok())
        .gateway("0.0.0.0".parse().ok())
        .tx_queues_cnt(0u16)
        .rx_queues_cnt(1u16)
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
        .name(String::from("My Rust Video Rx"))
        .build()
        .context("Failed to add rtp session")?;

    let mut video_rx = VideoRxBuilder::default()
        .netdev_id(0)
        .rtp_session(session)
        .width(args.width)
        .height(args.height)
        .fps(args.fps)
        .output_fmt(args.output_format)
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
    if display {
        sdl_context = sdl2::init().unwrap();
        video_subsystem = sdl_context.video().unwrap();
        window = video_subsystem
            .window("MTL RX Video", args.width / 4, args.height / 4)
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

    let frame_size = if let Some(output_format) = args.output_format {
        output_format.frame_size(args.width, args.height)?
    } else {
        video_rx.frame_size()
    };
    let frame = vec![0u8; frame_size];

    while running.load(Ordering::SeqCst) {
        match video_rx.fill_new_frame(&frame) {
            Ok(_) => {
                if let (Some(ref mut texture), Some(ref mut canvas)) = (&mut texture, &mut canvas) {
                    texture.update(None, &frame, args.width as usize * 2)?;
                    canvas.clear();
                    canvas.copy(texture, None, None).unwrap();
                    canvas.present();
                }
            }
            Err(_) => {
                video_rx.wait_new_frame();
            }
        }
    }

    // create a yuv file and save the frame to it
    if save_yuv {
        let file_name = args.yuv.unwrap();
        let mut yuv_file = std::fs::File::create(&file_name)?;
        yuv_file.write_all(&frame)?;
        println!("Wrote frame to yuv file {}", file_name);
    }

    Ok(())
}
