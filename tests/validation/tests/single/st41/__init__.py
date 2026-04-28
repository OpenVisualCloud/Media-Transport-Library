# SPDX-License-Identifier: BSD-3-Clause
# Copyright(c) 2024-2026 Intel Corporation

# Shared parametrize-id-to-value mappings for ST2110-41 (fastmetadata) tests.
# Keep values as ints — the RxTxApp JSON schema rejects strings for these fields.
payload_type_mapping = {
    "pt115": 115,
    "pt120": 120,
}

dit_mapping = {
    "dit0": 3648364,
    "dit1": 1234567,
}

k_bit_mapping = {
    "k0": 0,
    "k1": 1,
}
