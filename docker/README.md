1. ./script/nicctl.sh bind_kernel $PIC_ADDR
2. ./script/nicctl.sh create_vf $PIC_ADDR
3. ifconfig $VF_NIC 192.168.10.100/24  (ip a)
4. ./script/nicctl.sh bind_vf
