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
        const MTL_FLAG_BIND_NUMA                =
            sys::mtl_init_flag_MTL_FLAG_BIND_NUMA;
        const MTL_FLAG_PTP_ENABLE               =
            sys::mtl_init_flag_MTL_FLAG_PTP_ENABLE;
        const MTL_FLAG_RX_SEPARATE_VIDEO_LCORE  =
            sys::mtl_init_flag_MTL_FLAG_RX_SEPARATE_VIDEO_LCORE;
        const MTL_FLAG_TX_VIDEO_MIGRATE         =
            sys::mtl_init_flag_MTL_FLAG_TX_VIDEO_MIGRATE;
        const MTL_FLAG_RX_VIDEO_MIGRATE         =
            sys::mtl_init_flag_MTL_FLAG_RX_VIDEO_MIGRATE;
        const MTL_FLAG_TASKLET_THREAD           =
            sys::mtl_init_flag_MTL_FLAG_TASKLET_THREAD;
        const MTL_FLAG_TASKLET_SLEEP            =
            sys::mtl_init_flag_MTL_FLAG_TASKLET_SLEEP;
        const MTL_FLAG_RXTX_SIMD_512            =
            sys::mtl_init_flag_MTL_FLAG_RXTX_SIMD_512;
        const MTL_FLAG_PTP_PI                   =
            sys::mtl_init_flag_MTL_FLAG_PTP_PI;
        const MTL_FLAG_UDP_LCORE                =
            sys::mtl_init_flag_MTL_FLAG_UDP_LCORE;
        const MTL_FLAG_RANDOM_SRC_PORT          =
            sys::mtl_init_flag_MTL_FLAG_RANDOM_SRC_PORT;
        const MTL_FLAG_MULTI_SRC_PORT           =
            sys::mtl_init_flag_MTL_FLAG_MULTI_SRC_PORT;
        const MTL_FLAG_SHARED_TX_QUEUE          =
            sys::mtl_init_flag_MTL_FLAG_SHARED_TX_QUEUE;
        const MTL_FLAG_SHARED_RX_QUEUE          =
            sys::mtl_init_flag_MTL_FLAG_SHARED_RX_QUEUE;
        const MTL_FLAG_PHC2SYS_ENABLE           =
            sys::mtl_init_flag_MTL_FLAG_PHC2SYS_ENABLE;
        const MTL_FLAG_VIRTIO_USER              =
            sys::mtl_init_flag_MTL_FLAG_VIRTIO_USER;
        const MTL_FLAG_DEV_AUTO_START_STOP      =
            sys::mtl_init_flag_MTL_FLAG_DEV_AUTO_START_STOP;
        const MTL_FLAG_CNI_THREAD               =
            sys::mtl_init_flag_MTL_FLAG_CNI_THREAD;
        const MTL_FLAG_ENABLE_HW_TIMESTAMP      =
            sys::mtl_init_flag_MTL_FLAG_ENABLE_HW_TIMESTAMP;
        const MTL_FLAG_NIC_RX_PROMISCUOUS       =
            sys::mtl_init_flag_MTL_FLAG_NIC_RX_PROMISCUOUS;
        const MTL_FLAG_PTP_UNICAST_ADDR         =
            sys::mtl_init_flag_MTL_FLAG_PTP_UNICAST_ADDR;
        const MTL_FLAG_RX_MONO_POOL             =
            sys::mtl_init_flag_MTL_FLAG_RX_MONO_POOL;
        const MTL_FLAG_TASKLET_TIME_MEASURE     =
            sys::mtl_init_flag_MTL_FLAG_TASKLET_TIME_MEASURE;
        const MTL_FLAG_AF_XDP_ZC_DISABLE        =
            sys::mtl_init_flag_MTL_FLAG_AF_XDP_ZC_DISABLE;
        const MTL_FLAG_TX_MONO_POOL             =
            sys::mtl_init_flag_MTL_FLAG_TX_MONO_POOL;
        const MTL_FLAG_DISABLE_SYSTEM_RX_QUEUES =
            sys::mtl_init_flag_MTL_FLAG_DISABLE_SYSTEM_RX_QUEUES;
        const MTL_FLAG_PTP_SOURCE_TSC           =
            sys::mtl_init_flag_MTL_FLAG_PTP_SOURCE_TSC;
        const MTL_FLAG_TX_NO_CHAIN              =
            sys::mtl_init_flag_MTL_FLAG_TX_NO_CHAIN;
        const MTL_FLAG_TX_NO_BURST_CHK          =
            sys::mtl_init_flag_MTL_FLAG_TX_NO_BURST_CHK;
        const MTL_FLAG_RX_USE_CNI               =
            sys::mtl_init_flag_MTL_FLAG_RX_USE_CNI;
        const MTL_FLAG_RX_UDP_PORT_ONLY         =
            sys::mtl_init_flag_MTL_FLAG_RX_UDP_PORT_ONLY;
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
