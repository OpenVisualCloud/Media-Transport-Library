# ASan guide

AddressSanitizer(aka ASan, https://github.com/google/sanitizers/wiki/AddressSanitizer) is a fast memory error detector for C/C++ developed by Google. Media transport library use ASAN for memory related check. AddressSanitizer is a part of LLVM (3.1+) and GCC (4.8+). Enabling ASan is done by passing the -fsanitize=address option to the compiler flags.


Media transport library memory use DPDK memory related api to do the memory malloc/free, thus the error can be monitored is up to the DPDK ASan support.

## 1. Build DPDK with ASan detector.
DPDK introduce ASan support from DPDK 21.11 version.
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

## 2. Build Media transport library with ASan detector.
```bash
rm build/ -rf
ST_BUILD_ENABLE_ASAN=true ./build.sh
```

## 3. Run the app to check if any memory issues.
```bash
./build/app/RxTxApp --config_file tests/script/loop_json/1080p59_1v.json
```