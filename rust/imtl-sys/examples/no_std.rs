#![no_std]

use core::ptr::null_mut;
use imtl_sys::*;

fn main() {
    unsafe {
        let port_str = b"0000:4b:01.0" as *const _ as *const i8;
        let mut port_p = [0 as i8; 64];
        for i in 0..12 {
            port_p[i] = *port_str.add(i);
        }
        let mut port_param = mtl_port_init_params {
            flags: 0,
            socket_id: 0,
        };
        let mut param = mtl_init_params {
            port: [port_p; 8],
            num_ports: 1,
            pmd: [0; 8],
            sip_addr: [[192, 168, 96, 1]; 8],
            netmask: [[255, 255, 255, 0]; 8],
            gateway: [[192, 168, 96, 1]; 8],
            tx_sessions_cnt_max: 1,
            rx_sessions_cnt_max: 1,
            lcores: null_mut(),
            main_lcore: 0,
            dma_dev_port: [[0; 64]; 8],
            num_dma_dev_port: 0,
            log_level: mtl_log_level_MTL_LOG_LEVEL_INFO,
            flags: 0,
            priv_: null_mut(),
            ptp_get_time_fn: None,
            dump_period_s: 0,
            stat_dump_cb_fn: None,
            data_quota_mbs_per_sch: 0,
            nb_tx_desc: 0,
            nb_rx_desc: 0,
            pkt_udp_suggest_max_size: 0,
            nb_rx_hdr_split_queues: 0,
            rx_pool_data_size: 0,
            pacing: st21_tx_pacing_way_ST21_TX_PACING_WAY_AUTO,
            ki: 0.0,
            kp: 0.0,
            rss_mode: 0,
            iova_mode: 0,
            net_proto: [0; 8],
            tx_queues_cnt: [8; 8],
            rx_queues_cnt: [8; 8],
            tasklets_nb_per_sch: 8,
            tx_audio_sessions_max_per_sch: 8,
            rx_audio_sessions_max_per_sch: 8,
            arp_timeout_s: 0,
            ptp_sync_notify: None,
            rss_sch_nb: [0; 8],
            memzone_max: 0,
            port_params: [port_param; 8],
        };

        /* initialize dev handle */
        let dev_handle = mtl_init(&mut param as *mut _);
        if dev_handle == null_mut() {
            panic!("st_init fail");
        }

        /* destroy dev handle */
        mtl_uninit(dev_handle);
    }
}
