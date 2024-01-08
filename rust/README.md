# Rust

Bindings for IMTL in Rust.

## Usage

Add this to your `Cargo.toml`:

```toml
[dependencies]
imtl = "0.1.1"
```

## Example

Simple program to use IMTL to send raw YUV frame from file, video format: yuv42210bit 1080p60.

```bash
cargo run --example video-tx -- --yuv ./test.yuv
# Check more options with --help
cargo run --example video-tx -- --help
```
