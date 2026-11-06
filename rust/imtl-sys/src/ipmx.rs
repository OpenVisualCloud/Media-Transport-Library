// SPDX-License-Identifier: BSD-3-Clause
// Copyright 2026 Intel Corporation

//! IPMX (VSF TR-10) session flags; the `ipmx_api.h` functions and structs come with the
//! generated bindings. Needs an MTL built with enable_ipmx.

use crate::*;

/// Enable IPMX on an st20 TX session.
pub const ST20_TX_FLAG_IPMX: u32 = ipmx_sys_flag_IPMX_SYS_ST20_TX_FLAG;
/// Enable IPMX on an st20 RX session.
pub const ST20_RX_FLAG_IPMX: u32 = ipmx_sys_flag_IPMX_SYS_ST20_RX_FLAG;
/// Enable IPMX on an st22 TX session.
pub const ST22_TX_FLAG_IPMX: u32 = ipmx_sys_flag_IPMX_SYS_ST22_TX_FLAG;
/// Enable IPMX on an st22 RX session.
pub const ST22_RX_FLAG_IPMX: u32 = ipmx_sys_flag_IPMX_SYS_ST22_RX_FLAG;

// These tests need no NIC: they pin what the `ipmx` feature binds, not the library behavior.
#[cfg(test)]
mod tests {
    use crate::*;

    // The session flags are function-like macros bindgen cannot expand, so their values
    // come through wrapper_ipmx.h; pin them to the bits st20_api.h defines.

    /// ST20_TX_FLAG_IPMX is MTL_BIT32(12), st20_api.h.
    #[test]
    fn st20_tx_flag_value() {
        assert_eq!(ST20_TX_FLAG_IPMX, 1 << 12);
    }

    /// ST20_RX_FLAG_IPMX is MTL_BIT32(24), st20_api.h.
    #[test]
    fn st20_rx_flag_value() {
        assert_eq!(ST20_RX_FLAG_IPMX, 1 << 24);
    }

    /// ST22_TX_FLAG_IPMX is MTL_BIT32(9), st20_api.h.
    #[test]
    fn st22_tx_flag_value() {
        assert_eq!(ST22_TX_FLAG_IPMX, 1 << 9);
    }

    /// ST22_RX_FLAG_IPMX is MTL_BIT32(6), st20_api.h.
    #[test]
    fn st22_rx_flag_value() {
        assert_eq!(ST22_RX_FLAG_IPMX, 1 << 6);
    }

    // The ipmx_api.h surface is bound; the struct layouts are checked by the
    // bindgen_test_layout_ipmx_* tests of the generated bindings.

    /// The 22 ipmx_api.h functions are bound and link: taking their pointers pulls them from libmtl.
    #[test]
    fn functions_link() {
        let _functions = [
            ipmx_st20_rx_get_tp_stat as *const (),
            ipmx_st20p_rx_get_tp_stat as *const (),
            ipmx_st20_rx_get_sr_info as *const (),
            ipmx_st20p_rx_get_sr_info as *const (),
            ipmx_st22_rx_get_sr_info as *const (),
            ipmx_st22p_rx_get_sr_info as *const (),
            ipmx_st20_tx_set_block_version as *const (),
            ipmx_st20p_tx_set_block_version as *const (),
            ipmx_st20_tx_set_ts_refclk as *const (),
            ipmx_st20p_tx_set_ts_refclk as *const (),
            ipmx_st20_tx_get_info as *const (),
            ipmx_st20p_tx_get_info as *const (),
            ipmx_st22_tx_set_block_version as *const (),
            ipmx_st22p_tx_set_block_version as *const (),
            ipmx_st22_tx_set_ts_refclk as *const (),
            ipmx_st22p_tx_set_ts_refclk as *const (),
            ipmx_st22_tx_get_info as *const (),
            ipmx_st22p_tx_get_info as *const (),
            ipmx_st20_tx_get_sdp as *const (),
            ipmx_st20p_tx_get_sdp as *const (),
            ipmx_st22_tx_get_sdp as *const (),
            ipmx_st22p_tx_get_sdp as *const (),
        ];
    }

    /// The get_sdp buffer size the header guarantees is enough.
    #[test]
    fn sdp_max_value() {
        assert_eq!(IPMX_SDP_MAX, 2048);
    }
}
