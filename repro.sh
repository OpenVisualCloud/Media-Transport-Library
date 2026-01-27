#!/bin/bash


# Check if script is being run with ./repro.sh
if [[ "$0" != "./repro.sh" ]]; then
    echo "Error: This script must be run from the correct directory"
    exit 1
fi


 export INPUT_CONFIG_FILE=./tests/tools/RxTxApp/build/input.json

./cleanup.sh
./script/build_ice_driver.sh
./script/build_dpdk.sh
sudo ldconfig
./build.sh


# Check if ramdisk mount exists, create if it doesn't
if ! mountpoint -q /mnt/ramdisk; then
    mkdir -p /mnt/ramdisk
    mount -t tmpfs -o size=20G tmpfs /mnt/ramdisk
fi


FILE=/mnt/ramdisk/input.v210
if [ -f "$FILE" ]; then
    echo "$FILE exists."
else 
    echo "$FILE does not exist. Creating..."
    dd if=/dev/zero of=$FILE bs=1M count=1977 seek=0 2>/dev/null
fi



dpdk-devbind.py -u 0000:31:00.0
dpdk-devbind.py --bind ice 0000:31:00.0
./script/nicctl.sh create_vf 0000:31:00.0
sleep 4


./tests/tools/RxTxApp/build/RxTxApp --config_file $INPUT_CONFIG_FILE 
