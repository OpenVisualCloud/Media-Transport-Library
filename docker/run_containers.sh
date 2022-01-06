#!/bin/bash

if [ $# -lt 3 ]; then
    echo "Usage: "
    echo "    $0 <command> <bb:dd:ff.x> [Source ip] [Dest ip]"
    echo "Commands:"
    echo "   RX              receive"
    echo "   TX              Transmission"
	echo "<bb:dd:ff.x>:"
	echo "   ethtool -i Source_eth <bus_info>"
	echo "eg:"
	echo "   $0 tx 0000:1a:11.1 192.168.10.1 192.168.10.2"
    exit 0
fi

cmd=$1
BUS=$2
SIP=$3
DIP=$4

if [ $cmd == tx ] || [ $cmd == TX ]; then
	docker run --privileged --rm -it -u root --network host -v /dev/vfio/vfio:/dev/vfio/vfio  st2110app:v0.1 bash -c "ldconfig && ./build/app/RxTxApp  --p_port $BUS --p_sip $SIP --p_rx_ip $DIP --rx_video_sessions_count 1"
	
fi
	
if [ $cmd == rx ] || [ $cmd == RX ]; then
	docker run --privileged --rm -ti --network host -v /home/st2110/signal_be.yuv:/tmp/test.yuv:ro   st2110app:v0.1  bash -c "ldconfig && build/app/RxTxApp -m 16 --p_port $BUS --p_sip $SIP --p_tx_ip $DIP --tx_video_sessions_count 1 --tx_video_url /tmp/test.yuv"
	
fi


