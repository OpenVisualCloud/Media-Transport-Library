#!/bin/bash

if [ $# -lt 2 ]; then
    echo "Usage: "
    echo "    $0 ice-1.7.0_driver   ice-1.3.27.0.pkg  "
    echo "ICE-1.7.0_DRIVER_PATH:"
    echo "    eg: /<ICE_DRIVER_PATH>/ice-1.7.0_rc67_4_g614413e6_dirty"
    echo "ice-1.3.27.0.pkg:"
    echo "	  eg: /<ICE_PKG_PATH>/ice-1.3.27.0_mcast_hack_signed.pkg"
    exit 0
fi

ICE_DRIVER=$1
ICE_PKG=$2
DDP_PATH="/usr/lib/firmware/updates/intel/ice/ddp"
if [ ! -d "$ICE_DRIVER" ];then
  echo "ice-1.7.0 driver is not exits"
  exit 0
fi

if [ ! -f "$ICE_PKG" ];then
  echo "ice-1.3.27 pkg is not exits"
fi

if [ ! -d "$DDP_PATH" ];then
   mkdir -p $DDP_PATH
   if [ $? -ne 0 ]; then
       echo "Permission denied  <Please switch root>"
	   exit 0
   fi
fi

cd $ICE_DRIVER/src
	make
	make install
	rmmod ice
	modprobe ice

cd $DDP_PATH
cp  $ICE_PKG ./
rm ice.pkg
ln -s $ICE_PKG ice.pkg
rmmod ice
modprobe ice


