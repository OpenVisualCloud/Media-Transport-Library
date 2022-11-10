#!/bin/bash -ex

EX_PATH=${PWD}

build_ffmpeg(){
    if [ ! -d "./ffmpeg" ];then
        git clone https://git.ffmpeg.org/ffmpeg.git
    fi

    cd ./ffmpeg
    git checkout -b 4.4 origin/release/4.4
    git checkout 4.4
    git reset --hard aa28df74ab197c49a05fecc40c81e0f8ec4ad0c3
    cp -f ../kahawai.c ./libavdevice/
    git am --whitespace=fix ../0001-avdevice-kahawai-Add-the-kahawai-input-device-plugin.patch
    ./configure --enable-shared --disable-static --enable-nonfree --enable-pic --enable-gpl --enable-libst_dpdk
    make clean
    make -j32
    sudo make install
    cd ../
}

build_ffmpeg

echo "Building ffmpeg plugin is finished"
