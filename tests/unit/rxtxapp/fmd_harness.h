/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 *
 * RxTxApp in ST 2110-41 fast metadata mode, for the C++ tests: the json parser,
 * the st40p TX and RX app and the ancillary RX app run on fake lib calls.
 */

#ifndef _UT_RXTXAPP_FMD_HARNESS_H_
#define _UT_RXTXAPP_FMD_HARNESS_H_

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* the clock of the app sources */
void ut_rxtxapp_set_now(uint64_t ns);

/* tx_st40p_app.c, a fake st40p_tx_put_frame records each frame */
typedef struct ut_tx_st40p ut_tx_st40p;
/* json: one tx st40p session, NULL if app_tx_st40p_init fails */
ut_tx_st40p* ut_tx_st40p_create(const char* json);
uint32_t ut_tx_st40p_ops_flags(ut_tx_st40p* t);
/* runs app_tx_st40p_frame_thread for n frames, returns the frames put */
int ut_tx_st40p_run(ut_tx_st40p* t, int n);
/* frame i as the lib sends it: the data item, zero padded to a word */
uint32_t ut_tx_st40p_item(ut_tx_st40p* t, int i, uint8_t* out, uint32_t cap);
uint32_t ut_tx_st40p_fill(ut_tx_st40p* t, int i);
void ut_tx_st40p_free(ut_tx_st40p* t);

/* rx_st40p_app.c, a fake st40p_rx_get_frame gives the pushed items */
typedef struct ut_rx_st40p ut_rx_st40p;
/* json: one rx st40p session, NULL if app_rx_st40p_init fails */
ut_rx_st40p* ut_rx_st40p_create(const char* json);
uint32_t ut_rx_st40p_ops_flags(ut_rx_st40p* r);
/* makes the fake st40p_rx_create fail */
void ut_rx_st40p_fail_create(bool fail);
bool ut_rx_st40p_ref_mapped(ut_rx_st40p* r);
void ut_rx_st40p_push(ut_rx_st40p* r, const uint8_t* item, uint32_t fill, uint32_t dit,
                      uint8_t k_bit, bool marker, bool seq_discont, uint32_t seq_lost,
                      uint64_t at_ns);
/* runs app_rx_st40p_frame_thread over the pushed items */
void ut_rx_st40p_run(ut_rx_st40p* r);
int ut_rx_st40p_received(ut_rx_st40p* r);
int ut_rx_st40p_invalid(ut_rx_st40p* r);
/* app_rx_st40p_result at the harness clock */
int ut_rx_st40p_result(ut_rx_st40p* r);
void ut_rx_st40p_free(ut_rx_st40p* r);

/* rx_ancillary_app.c, a fake st40_rx_get_mbuf gives the pushed packets */
typedef struct ut_rx_anc ut_rx_anc;
/* json: one rx ancillary session, NULL if app_rx_anc_init fails */
ut_rx_anc* ut_rx_anc_create(const char* json);
/* one fast metadata packet with the DIT and K-bit of the json */
void ut_rx_anc_push(ut_rx_anc* r, uint16_t seq, const uint8_t* item, uint16_t padded,
                    uint64_t at_ns);
/* runs app_rx_anc_read_thread over the pushed packets */
void ut_rx_anc_run(ut_rx_anc* r);
int ut_rx_anc_invalid(ut_rx_anc* r);
/* app_rx_anc_result at the harness clock */
int ut_rx_anc_result(ut_rx_anc* r);
void ut_rx_anc_free(ut_rx_anc* r);

/* st_json_parse_{tx,rx}_{st40p,anc} of parse_json.c */
struct ut_fmd_json {
  int ret;
  bool fast_metadata;
  int dit;
  int k_bit;
  int payload_type;
  char url[256];
};
void ut_parse_st40p(const char* json, bool tx, struct ut_fmd_json* out);
void ut_parse_anc(const char* json, bool tx, struct ut_fmd_json* out);

#ifdef __cplusplus
}
#endif

#endif
