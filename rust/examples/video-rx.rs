use anyhow::{Context, Result};
use clap::Parser;
use std::io::Write;
use std::net::Ipv4Addr;
use std::rc::Rc;
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::Arc;

use imtl::mtl::{Flags, LogLevel, MtlBuilder};
use imtl::netdev::*;
use imtl::session::RtpSessionBuilder;
use imtl::video::{Fps, TransportFmt, VideoRxBuilder};

/// Simple program to use IMTL to receive raw YUV frame and safe the latest one to file
#[derive(Parser, Debug)]
#[command(author, version, about, long_about = None)]
struct Args {
    /// Name of the netdev
    #[arg(short, long, default_value_t = String::from("0000:4b:01.1"))]
    netdev: String,

    /// Netdev IP address
    #[arg(short, long, default_value_t = Ipv4Addr::new(192, 168, 96, 112))]
    sip: Ipv4Addr,

    /// Destination IP address
    #[arg(short, long, default_value_t = Ipv4Addr::new(239, 19, 96, 111))]
    ip: Ipv4Addr,

    /// Destination UDP Port number
    #[arg(short, long, default_value_t = 20000)]
    port: u16,

    /// Name of the YUV file
    #[arg(short, long)]
    yuv: String,
}

fn main() -> Result<()> {
    let args = Args::parse();

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
        .log_level(LogLevel::Info)
        .build()
        .unwrap()
        .init()
        .context("Failed to init mtl")?;

    let net_dev0 = Rc::new(mtl.net_devs()[0].clone());

    let session = RtpSessionBuilder::default()
        .net_dev(net_dev0.clone())
        .ip(args.ip)
        .port(args.port)
        .payload_type(112u8)
        .name(String::from("My Rust Video Rx"))
        .build()
        .context("Failed to add rtp session")?;

    let mut video_rx = VideoRxBuilder::default()
        .rtp_session(session)
        .width(1920u32)
        .height(1080u32)
        .fps(Fps::P60)
        .t_fmt(TransportFmt::Yuv422_10bit)
        .build()
        .unwrap()
        .create(&mtl)
        .context("Failed to create tx video session")?;

    let frame = vec![0u8; video_rx.frame_size()];

    while running.load(Ordering::SeqCst) {
        match video_rx.fill_new_frame(&frame) {
            Ok(_) => {}
            Err(_) => {
                video_rx.wait_new_frame();
            }
        }
    }

    // create a yuv file and save the frame to it
    let mut yuv_file = std::fs::File::create(&args.yuv)?;
    yuv_file.write_all(&frame)?;
    println!("Wrote frame to yuv file {}", &args.yuv);

    Ok(())
}
