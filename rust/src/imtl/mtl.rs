/*!
 * MTL Main Handle
 *
 * This module provides the foundational components for the MTL library.
 * It defines configurations for logging, networking protocols, IOVA modes, and
 * initializable structures needed to create and manage network devices and their
 * behaviors in a flexible manner. The `mtl` structure serves as the main handle
 * for interacting with the library, providing methods for initialization and management.
 *
 * The module utilizes bitflags for managing various initialization flags and derives builders
 * for easy and readable instance creation. Error handling is facilitated by the `anyhow` crate.
 *
 * Enums `LogLevel`, `RssMode`, and `IovaMode` define the operation modes for logging,
 * Receive Side Scaling, and input-output virtual address translation, respectively.
 * Struct `Flags` provides bitflag constants for fine-grained initialization options. The `Mtl`
 * struct encapsulates the library configuration and is designed with a builder pattern
 * for convenient construction.
 *
 * The `Mtl` also implements the `Drop` trait to ensure proper cleanup and resource deallocation,
 * further promoting safety and preventing resource leaks.
 *
 * Usage of this library requires careful composition of these configurations to match
 * the specific needs of the network environment being targeted.
 */
use anyhow::{bail, Result};
use bitflags::bitflags;
use derive_builder::Builder;
use std::mem::MaybeUninit;

use crate::netdev::NetDev;
use crate::sys;

/// Level of detail for logging messages.
#[derive(Copy, Clone, Debug, Default)]
pub enum LogLevel {
    Debug = 0,
    Info,
    Notice,
    Warning,
    /// Default logging level, indicating an error.
    #[default]
    Error,
}

/// Receive Side Scaling (RSS) mode options for the network device.
#[derive(Copy, Clone, Debug, Default)]
pub enum RssMode {
    #[default]
    None = 0,
    L3,
    L3L4,
}

/// DPDK IOVA mode.
#[derive(Copy, Clone, Debug, Default)]
pub enum IovaMode {
    /// Default mode, automatically determines address translation.
    #[default]
    Auto = 0,
    /// Use IOVA virtual address mode.
    Va,
    /// Use IOVA physical address mode.
    Pa,
}

// Define a set of flags using the `bitflags` macro. These flags will be used
// to set configuration options for initializing MTL.
bitflags! {
    #[derive(Copy, Clone, Debug, Default)]
    pub struct Flags: u64 {
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

/// Configuration structure for a new `mtl` instance.
#[derive(Default, Builder, Debug, Clone)]
#[builder(setter(into))]
pub struct Mtl {
    /// Handle to the `mtl` instance after initialization.
    #[builder(default)]
    handle: Option<sys::mtl_handle>,

    /// Mandatory list of network devices.
    net_devs: Vec<NetDev>, /* Mandatory */
    /// Optional list of DMA devices.
    #[builder(default)]
    dma_devs: Vec<String>,
    /// Optional list of logical cores to pin the `mtl` schedulers to.
    #[builder(default)]
    lcores: Option<String>,
    /// Optional logging level configuration.
    #[builder(default)]
    log_level: LogLevel,
    /// Optional RSS configuration.
    #[builder(default)]
    rss_mode: RssMode,
    /// Optional IOVA mode configuration.
    #[builder(default)]
    iova_mode: IovaMode,
    /// Optional flags configuration.
    #[builder(default)]
    flags: Flags,
}

impl Mtl {
    /// Initialize the 'mtl' instance with the provided configuration.
    ///
    /// # Errors
    /// Returns an error if the 'mtl' has already been initialized or if the number of network devices
    /// is not within the allowed range.
    pub fn init(mut self) -> Result<Self> {
        if self.handle.is_some() {
            bail!("MTL is already initialized");
        }

        let num_ports = self.net_devs.len();
        if num_ports > 8 || num_ports == 0 {
            bail!("Invalid number of netdevs");
        }

        // Create an uninitialized instance of mtl_init_params and zero out the memory
        let mut c_param: MaybeUninit<sys::mtl_init_params> = MaybeUninit::uninit();

        // Fill the params
        unsafe {
            std::ptr::write_bytes(c_param.as_mut_ptr(), 0, 1);
            let c_param = &mut *c_param.as_mut_ptr();
            c_param.num_ports = num_ports as _;
            for (i, net_dev) in self.net_devs.iter().enumerate() {
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
                if let Some(ip) = net_dev.get_ip() {
                    c_param.sip_addr[i] = ip.octets();
                }
                if let Some(ip) = net_dev.get_netmask() {
                    c_param.netmask[i] = ip.octets();
                }
                if let Some(ip) = net_dev.get_gateway() {
                    c_param.gateway[i] = ip.octets();
                }
                c_param.tx_queues_cnt[i] = net_dev.get_tx_queues_cnt();
                c_param.rx_queues_cnt[i] = net_dev.get_rx_queues_cnt();
            }
            for (i, dma_dev) in self.dma_devs.iter().enumerate() {
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

            c_param.log_level = self.log_level as _;
            c_param.rss_mode = self.rss_mode as _;
            c_param.iova_mode = self.iova_mode as _;
            if let Some(lcores) = &self.lcores {
                c_param.lcores = lcores.as_ptr() as _;
            }
            c_param.flags = self.flags.bits();
        }

        let mut c_param = unsafe { c_param.assume_init() };

        let handle = unsafe { sys::mtl_init(&mut c_param as *mut _) };
        if handle == std::ptr::null_mut() {
            bail!("Failed to initialize MTL")
        } else {
            self.handle = Some(handle);
            Ok(self)
        }
    }

    /// Get a reference to the `mtl` handle.
    pub fn handle(&self) -> &Option<sys::mtl_handle> {
        &self.handle
    }

    /// Get a reference to the vector of `NetDev` configuration instances.
    pub fn net_devs(&self) -> &Vec<NetDev> {
        &self.net_devs
    }
}

// Drop trait implementation to automatically clean up resources when the `Mtl` instance goes out of scope.
impl Drop for Mtl {
    fn drop(&mut self) {
        if let Some(handle) = self.handle {
            unsafe {
                sys::mtl_uninit(handle);
            }
        }
    }
}
