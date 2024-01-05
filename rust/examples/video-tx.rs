use anyhow::{Context, Ok, Result};
use std::net::Ipv4Addr;
use std::rc::Rc;
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::Arc;

use imtl::mtl::{Flags, LogLevel, MtlBuilder};
use imtl::netdev::*;
use imtl::session::RtpSessionBuilder;
use imtl::video::{Fps, TransportFmt, VideoTxBuilder};

fn main() -> Result<()> {
    let running = Arc::new(AtomicBool::new(true));
    let r = running.clone();
    ctrlc::set_handler(move || {
        r.store(false, Ordering::SeqCst);
    })
    .expect("Error setting Ctrl-C handler");

    let mut flags = Flags::empty();
    flags.insert(Flags::MTL_FLAG_BIND_NUMA | Flags::MTL_FLAG_DEV_AUTO_START_STOP);

    let net_dev0 = NetDevBuilder::default()
        .port("0000:4b:01.0")
        .pmd(PmdType::DpdkPmd)
        .net_proto(NetProto::Static)
        .ip("192.168.96.111".parse().ok())
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
        .log_level(LogLevel::Info)
        .build()
        .unwrap()
        .init()
        .context("Failed to init mtl")?;

    let net_dev0 = Rc::new(mtl.net_devs()[0].clone());

    let session = RtpSessionBuilder::default()
        .net_dev(net_dev0.clone())
        .ip(Ipv4Addr::new(239, 19, 96, 111))
        .port(20000u16)
        .payload_type(112u8)
        .name("My Rust Video Tx".to_string())
        .build()
        .context("Failed to add rtp session")?;

    let _video_tx = VideoTxBuilder::default()
        .rtp_session(session)
        .width(1920u32)
        .height(1080u32)
        .fps(Fps::P60)
        .t_fmt(TransportFmt::Yuv422_10bit)
        .build()
        .unwrap()
        .create(&mtl)
        .context("Failed to create tx video session")?;

    while running.load(Ordering::SeqCst) {
        std::thread::sleep(std::time::Duration::from_millis(100));
    }

    Ok(())
}
