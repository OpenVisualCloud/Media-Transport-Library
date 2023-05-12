# ASan guide

AddressSanitizer (also known as ASan, <https://github.com/google/sanitizers/wiki/AddressSanitizer>) is a fast memory error detector for C/C++ developed by Google. The Intel® Media Transport Library uses ASan for memory-related checks. ASan is a part of LLVM (version 3.1+) and GCC (version 4.8+). To enable ASan, pass the -fsanitize=address option to the compiler flags.

The library uses DPDK API to perform memory malloc/free operations. Therefore, the error monitoring capability depends on the DPDK ASan support.

## 1. Build DPDK with ASan detector

To use ASan with DPDK, you must build DPDK with ASan support. ASan support was introduced in DPDK version 21.11

```bash
rm build -rf
meson build -Db_sanitize=address -Dbuildtype=debug
ninja -C build
cd build
sudo ninja install
pkg-config --cflags libdpdk
pkg-config --libs libdpdk
pkg-config --modversion libdpdk
```

## 2. Build Intel® Media Transport Library with ASan detector

```bash
rm build/ -rf
ST_BUILD_ENABLE_ASAN=true ./build.sh
```

## 3. Run the application to check for any memory issues

```bash
./build/app/RxTxApp --config_file tests/script/loop_json/1080p59_1v.json
```
