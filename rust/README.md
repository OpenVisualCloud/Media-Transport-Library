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
