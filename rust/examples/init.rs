use imtl::mtl::{Mtl, MtlInitParams, MtlLogLevel};
use imtl::netdev::*;

fn main() {
    let mut params = MtlInitParams::new();

    let net_dev = MtlNetDev::new(
        "0000:4b:01.0".to_string(),
        MtlNetProto::Static,
        MtlPmdType::DpdkPmd,
        "192.168.96.111".parse().ok(),
        "255.255.255.0".parse().ok(),
        "0.0.0.0".parse().ok(),
        1,
        1,
        1,
    );
    params.add_netdev(net_dev);
    params.set_log_level(MtlLogLevel::Debug);

    let _mtl = Mtl::new(&params);
    std::thread::sleep(std::time::Duration::from_secs(3));
}
