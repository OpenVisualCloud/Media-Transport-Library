use anyhow::{bail, Context, Result};
use clap::Parser;
use std::net::Ipv4Addr;
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::Arc;

use imtl::audio::{AudioTxBuilder, Fmt, PacketTime, Sampling};
use imtl::mtl::{Flags, LogLevel, MtlBuilder};
use imtl::netdev::*;
use imtl::session::RtpSessionBuilder;

/// Simple program to use IMTL to send raw audio from file
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

    /// Audio format
    #[arg(long, default_value_t = Fmt::Pcm16)]
    fmt: Fmt,

    /// Audio channel
    #[arg(long, default_value_t = 2)]
    channel: u16,

    /// Audio sampling
    #[arg(long, default_value_t = Sampling::S48K)]
    sampling: Sampling,

    /// Audio packet time
    #[arg(long, default_value_t = PacketTime::P1ms)]
    ptime: PacketTime,

    /// Audio frametime ms
    #[arg(long, default_value_t = 10)]
    frametime: u16,

    /// Name of the pcm file
    #[arg(long)]
    pcm: String,

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

    /* open a pcm file from disk and map to memory */
    let pcm_file = std::fs::File::open(args.pcm)?;
    let pcm_file = unsafe { memmap2::MmapOptions::new().map(&pcm_file)? };

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
        .name(String::from("My Rust Audio Tx"))
        .build()
        .context("Failed to add rtp session")?;

    let mut audio_tx = AudioTxBuilder::default()
        .netdev_id(0)
        .rtp_session(session)
        .fmt(args.fmt)
        .channel(args.channel)
        .sampling(args.sampling)
        .ptime(args.ptime)
        .frametime_ms(args.frametime)
        .build()
        .unwrap()
        .create(&mtl)
        .context("Failed to create tx audio session")?;

    let frame_size = audio_tx.frame_size();
    let frames = pcm_file.chunks_exact(frame_size);
    if frames.len() == 0 {
        bail!("No frames in file");
    }
    let mut frames = frames.cycle();
    let mut frame = frames.next().unwrap();

    while running.load(Ordering::SeqCst) {
        if audio_tx.fill_next_frame(frame).is_ok() {
            frame = frames.next().unwrap();
        }
    }

    Ok(())
}
