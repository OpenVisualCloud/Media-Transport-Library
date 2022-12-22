#!/bin/bash -ex

EX_PATH=${PWD}

build_openh264(){
    if [ ! -d "./openh264" ];then
        git clone https://github.com/cisco/openh264.git
    fi

    cd ./openh264
    git checkout -b openh264v2.3.1 origin/openh264v2.3.1
    make -j32
    sudo make install
    sudo ldconfig
    cd ../
}

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
    ./configure --enable-shared --disable-static --enable-nonfree --enable-pic --enable-gpl --enable-mtl --enable-libopenh264 --enable-encoder=libopenh264
    make clean
    make -j32
    sudo make install
    sudo ldconfig
    cd ../
}

build_openh264
build_ffmpeg

echo "Building ffmpeg plugin is finished"
