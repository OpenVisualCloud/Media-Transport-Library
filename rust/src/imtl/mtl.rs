use bitflags::bitflags;
use std::mem::MaybeUninit;
use std::ptr::null_mut;

use crate::netdev::MtlNetDev;
use crate::sys;

#[derive(Copy, Clone)]
pub enum MtlLogLevel {
    Debug = 0,
    Info,
    Notice,
    Warning,
    Error,
}

#[derive(Copy, Clone)]
pub enum MtlRssMode {
    None = 0,
    L3,
    L3L4,
}

#[derive(Copy, Clone)]
pub enum MtlIovaMode {
    Auto = 0,
    Va,
    Pa,
}

bitflags! {
    pub struct MtlFlags: u64 {
        const MTL_FLAG_BIND_NUMA                = 1 << 0;
        const MTL_FLAG_PTP_ENABLE               = 1 << 1;
        const MTL_FLAG_RX_SEPARATE_VIDEO_LCORE  = 1 << 2;
        const MTL_FLAG_TX_VIDEO_MIGRATE         = 1 << 3;
        const MTL_FLAG_RX_VIDEO_MIGRATE         = 1 << 4;
        const MTL_FLAG_TASKLET_THREAD           = 1 << 5;
        const MTL_FLAG_TASKLET_SLEEP            = 1 << 6;
        const MTL_FLAG_RXTX_SIMD_512            = 1 << 7;
        const MTL_FLAG_PTP_PI                   = 1 << 9;
        const MTL_FLAG_UDP_LCORE                = 1 << 10;
        const MTL_FLAG_RANDOM_SRC_PORT          = 1 << 11;
        const MTL_FLAG_MULTI_SRC_PORT           = 1 << 12;
        const MTL_FLAG_SHARED_TX_QUEUE          = 1 << 13;
        const MTL_FLAG_SHARED_RX_QUEUE          = 1 << 14;
        const MTL_FLAG_PHC2SYS_ENABLE           = 1 << 15;
        const MTL_FLAG_VIRTIO_USER              = 1 << 16;
        const MTL_FLAG_DEV_AUTO_START_STOP      = 1 << 17;
        const MTL_FLAG_CNI_THREAD               = 1 << 32;
        const MTL_FLAG_ENABLE_HW_TIMESTAMP      = 1 << 33;
        const MTL_FLAG_NIC_RX_PROMISCUOUS       = 1 << 34;
        const MTL_FLAG_PTP_UNICAST_ADDR         = 1 << 35;
        const MTL_FLAG_RX_MONO_POOL             = 1 << 36;
        const MTL_FLAG_TASKLET_TIME_MEASURE     = 1 << 38;
        const MTL_FLAG_AF_XDP_ZC_DISABLE        = 1 << 39;
        const MTL_FLAG_TX_MONO_POOL             = 1 << 40;
        const MTL_FLAG_DISABLE_SYSTEM_RX_QUEUES = 1 << 41;
        const MTL_FLAG_PTP_SOURCE_TSC           = 1 << 42;
        const MTL_FLAG_TX_NO_CHAIN              = 1 << 43;
        const MTL_FLAG_TX_NO_BURST_CHK          = 1 << 44;
        const MTL_FLAG_RX_USE_CNI               = 1 << 45;
        const MTL_FLAG_RX_UDP_PORT_ONLY         = 1 << 46;
    }
}

pub struct MtlInitParams {
    net_devs: Vec<MtlNetDev>,
    dma_devs: Vec<String>,
    lcores: Option<String>,
    log_level: MtlLogLevel,
    rss_mode: MtlRssMode,
    iova_mode: MtlIovaMode,
    flags: MtlFlags,
}

impl MtlInitParams {
    pub fn new() -> MtlInitParams {
        MtlInitParams {
            net_devs: Vec::new(),
            dma_devs: Vec::new(),
            lcores: None,
            log_level: MtlLogLevel::Info,
            rss_mode: MtlRssMode::None,
            iova_mode: MtlIovaMode::Auto,
            flags: MtlFlags::empty(),
        }
    }
    pub fn add_netdev(&mut self, netdev: MtlNetDev) {
        self.net_devs.push(netdev);
    }
    pub fn add_dma_dev(&mut self, dma_dev: String) {
        self.dma_devs.push(dma_dev);
    }
    pub fn set_lcores(&mut self, lcores: String) {
        self.lcores = Some(lcores);
    }
    pub fn set_log_level(&mut self, log_level: MtlLogLevel) {
        self.log_level = log_level;
    }
    pub fn set_rss_mode(&mut self, rss_mode: MtlRssMode) {
        self.rss_mode = rss_mode;
    }
    pub fn set_iova_mode(&mut self, iova_mode: MtlIovaMode) {
        self.iova_mode = iova_mode;
    }
    pub fn set_flags(&mut self, flags: MtlFlags) {
        self.flags = flags;
    }
}

pub struct Mtl {
    handle: sys::mtl_handle,
}

impl Mtl {
    pub fn new(params: &MtlInitParams) -> Result<Mtl, &'static str> {
        let num_ports = params.net_devs.len();
        if num_ports > 8 || num_ports == 0 {
            return Err("Invalid number of netdevs");
        }

        // Create an uninitialized instance of mtl_init_params and zero out the memory
        let mut c_param: MaybeUninit<sys::mtl_init_params> = MaybeUninit::uninit();
        unsafe {
            std::ptr::write_bytes(c_param.as_mut_ptr(), 0, 1);
        }

        // Fill the params
        unsafe {
            let c_param = &mut *c_param.as_mut_ptr();
            c_param.num_ports = num_ports as _;
            for (i, net_dev) in params.net_devs.iter().enumerate() {
                let port_bytes: Vec<i8> = net_dev
                    .get_port()
                    .as_bytes()
                    .iter()
                    .cloned()
                    .map(|b| b as i8) // Convert u8 to i8
                    .chain(std::iter::repeat(0)) // Pad with zeros if needed
                    .take(64) // Take only up to 64 elements
                    .collect();
                c_param.port[i].copy_from_slice(&port_bytes);

                c_param.pmd[i] = net_dev.get_pmd() as _;
                c_param.net_proto[i] = net_dev.get_net_proto() as _;
                if let Some(ip) = net_dev.get_sip_addr() {
                    c_param.sip_addr[i] = ip.octets();
                }
                if let Some(ip) = net_dev.get_netmask() {
                    c_param.netmask[i] = ip.octets();
                }
                if let Some(ip) = net_dev.get_gateway() {
                    c_param.gateway[i] = ip.octets();
                }
            }
            for (i, dma_dev) in params.dma_devs.iter().enumerate() {
                let port_bytes: Vec<i8> = dma_dev
                    .as_bytes()
                    .iter()
                    .cloned()
                    .map(|b| b as i8) // Convert u8 to i8
                    .chain(std::iter::repeat(0)) // Pad with zeros if needed
                    .take(64) // Take only up to 64 elements
                    .collect();
                c_param.dma_dev_port[i].copy_from_slice(&port_bytes);
                c_param.num_dma_dev_port += 1;
            }

            c_param.log_level = params.log_level as _;
            c_param.rss_mode = params.rss_mode as _;
            c_param.iova_mode = params.iova_mode as _;
            if let Some(lcores) = &params.lcores {
                c_param.lcores = lcores.as_ptr() as _;
            }
            c_param.flags = params.flags.bits();
        }

        let mut c_param = unsafe { c_param.assume_init() };

        let handle = unsafe { sys::mtl_init(&mut c_param as *mut _) };
        if handle == null_mut() {
            Err("Failed to initialize MTL")
        } else {
            Ok(Mtl { handle })
        }
    }
}

impl Drop for Mtl {
    fn drop(&mut self) {
        unsafe {
            sys::mtl_uninit(self.handle);
        }
    }
}
