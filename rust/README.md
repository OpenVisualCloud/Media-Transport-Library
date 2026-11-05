# Rust

Bindings for MTL in Rust.

## Usage

Add this to your `Cargo.toml`:

```toml
[dependencies]
imtl = "0.1.3"
```

## Example

Simple program to use MTL to send raw YUV frame from file.

```bash
cargo run --example video-tx -- --yuv /tmp/test.yuv --width 1920 --height 1080 --fps 30 --format yuv_422_8bit
# Check more options with --help
cargo run --example video-tx -- --help
```

Simple program to use MTL to receive raw YUV frame and display or save the latest one to file.

```bash
cargo run --example video-rx -- --display --width 1920 --height 1080 --fps 30 --format yuv_422_8bit [--yuv /tmp/save.yuv]
# Check more options with --help
cargo run --example video-rx -- --help
```

USe pipeline API for internal color format conversion by adding '--input_format' for Tx and '--output_format' for Rx.

```bash
cargo run --example video-tx -- --yuv 422p10le.yuv --width 1920 --height 1080 --fps 30 --format yuv_422_10bit --input-format YUV422PLANAR10LE
cargo run --example video-rx -- --display --width 1920 --height 1080 --fps 30 --format yuv_422_10bit --output-format UYVY
```

## IPMX

The `ipmx` feature of `imtl-sys` (off by default) adds the IPMX session flags `ST20_TX_FLAG_IPMX`, `ST20_RX_FLAG_IPMX`, `ST22_TX_FLAG_IPMX`, `ST22_RX_FLAG_IPMX` and the functions and structs of `ipmx_api.h`, among them `ipmx_st20p_tx_get_sdp()` and `ipmx_st20p_tx_set_block_version()`.
The pipeline flags (`st20p_tx_flag_ST20P_TX_FLAG_IPMX` etc.) are there without the feature. The safe `imtl` crate has no IPMX support.

The feature needs an MTL built with `enable_ipmx`: the build fails when `pkg-config --variable=ipmx mtl` is not `true`. It exists only in this tree, so use a path dependency:

```toml
[dependencies]
imtl-sys = { path = "<mtl>/rust/imtl-sys", features = ["ipmx"] }
```

`ipmx_st20p_tx_get_sdp()` writes a NUL terminated SDP into a buffer of `IPMX_SDP_MAX` bytes, which is always enough, and returns its length (>0) or a negative error; read the buffer only when the return is >0, on an error the library may leave it untouched. Poll it and republish the SDP when its session version changes.
When the application re-creates a session with stream parameters that change the Info Block content, it reads the block version of the old session with `ipmx_st20p_tx_get_info()` and sets that + 1, modulo 256, with `ipmx_st20p_tx_set_block_version()` before the first `st20p_tx_put_frame()`.

As in C, a NULL handle crashes the process.
