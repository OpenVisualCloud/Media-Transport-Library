/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 */
/* A hand-formatted design sketch: formatted when it moves to include/ (milestone M0). */
/* clang-format off */
/*
 * mtl_crypto.h - payload encryption (IPMX PEP, TR-10-13), revision 0.2.
 *
 * Set the option crypto.scheme (mtl_options.h, keys crypto.*) and the session encrypts
 * (TX) or decrypts (RX) its payloads with AES in counter mode. The parameters are options,
 * none of them secret, so an IS-05 activation can change them at its boundary and the SDP
 * helper can render and parse them. The library writes and parses the RFC 8285 counter
 * extensions, keeps the counters, sizes packets for the extension, and runs the cipher off
 * the tasklet: in the caller at submit and dequeue (both become DPC), or on crypto.workers
 * threads. ST 2022-7 legs carry the same ciphertext. Keys never are options: they go in
 * with mtl_crypto_set_key(), are kept in locked, non-dumpable memory, zeroised on replace
 * and close, and never reach logs, stats or captures. Key derivation (pre-shared keys,
 * KDF, ECDH) stays with the application, which must use a fresh key generator per boot. A
 * TX unit without a key is DROPPED (NO_KEY), never sent in clear; an RX packet that fails
 * authentication is a lost packet ("rx.crypto_auth_fail"). HDCP keys never enter MTL:
 * packet units, with the vendor's code encrypting, carry HDCP streams.
 */

#ifndef MTL_EXPERIMENTAL_MTL_CRYPTO_H
#define MTL_EXPERIMENTAL_MTL_CRYPTO_H

#include "mtl.h"

#if defined(__cplusplus)
extern "C" {
#endif

#if defined(MTL_LATER) /* a later phase (implementation-plan.md §6); the design is kept */
/* Values of crypto.scheme and crypto.mode. */
enum mtl_crypto_scheme {
  MTL_CRYPTO_PEP_RTP = 1,    /* one key per activation */
  MTL_CRYPTO_PEP_RTP_KV = 2, /* key versions, switched at unit boundaries */
};
enum mtl_crypto_mode {
  MTL_CRYPTO_AES128_CTR = 1, /* mandatory in TR-10-13 */
  MTL_CRYPTO_AES256_CTR = 2,
  MTL_CRYPTO_AES128_CTR_CMAC64 = 3, /* MAC modes always take the copy path */
  MTL_CRYPTO_AES256_CTR_CMAC64 = 4,
  MTL_CRYPTO_AES128_CTR_CMAC64_AAD = 5,
  MTL_CRYPTO_AES256_CTR_CMAC64_AAD = 6,
};

/* Installs key `key_version` (16 or 32 bytes, copied); key NULL zeroises every key, and TX
   units are then DROPPED (NO_KEY). TX: used from the first unit at or after `when` (NULL =
   the next unit). The counter restarts at 0 only for key bytes this session never used;
   re-installing a used key continues its counter, so no IV and counter pair repeats
   under one key. RX: applies to units whose media time is at or after `when` (the update
   rule of mtl.h), kept beside the current key and chosen per packet by the extension's key
   version (RTP_KV); a packet with an unknown version counts in "rx.crypto_unknown_key"
   and posts MTL_EVENT_KEY_NEEDED. Never readable back. CP. */
MTL_API_CP int mtl_crypto_set_key(mtl_session_h s, uint32_t key_version,
                                  const uint8_t* MTL_NULLABLE key, uint32_t key_bytes,
                                  const struct mtl_when* MTL_NULLABLE when);
#endif

#if defined(__cplusplus)
}
#endif

/* clang-format on */
#endif /* MTL_EXPERIMENTAL_MTL_CRYPTO_H */
