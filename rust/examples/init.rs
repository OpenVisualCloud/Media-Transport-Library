use imtl::mtl::{Flags, LogLevel, MtlBuilder};
use imtl::netdev::*;

fn main() {
    let mut flags = Flags::empty();
    flags.insert(Flags::MTL_FLAG_BIND_NUMA);

    let net_dev0 = NetDevBuilder::default()
        .port("0000:4b:01.0")
        .pmd(PmdType::DpdkPmd)
        .net_proto(NetProto::Static)
        .ip("192.168.96.111".parse().ok())
        .netmask("255.255.255.0".parse().ok())
        .gateway("0.0.0.0".parse().ok())
        .tx_queues_cnt(1u16)
        .rx_queues_cnt(1u16)
        .build()
        .unwrap();

    let net_dev1 = NetDevBuilder::default()
        .port("0000:4b:01.1")
        .pmd(PmdType::DpdkPmd)
        .net_proto(NetProto::Dhcp)
        .tx_queues_cnt(1u16)
        .rx_queues_cnt(1u16)
        .build()
        .unwrap();

    let net_devs = vec![net_dev0, net_dev1];

    let _mtl = MtlBuilder::default()
        .net_devs(net_devs)
        .flags(flags)
        .log_level(LogLevel::Info)
        .build()
        .unwrap()
        .init();
    std::thread::sleep(std::time::Duration::from_secs(3));
}
