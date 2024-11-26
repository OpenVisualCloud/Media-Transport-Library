import subprocess


def check_socket(bound_list, dma_bound_list):
    bound_list.sort()
    dma_bound_list.sort()
    socket_0_list = []
    socket_1_list = []
    dma_socket_0_list = []
    dma_socket_1_list = []
    for nic_port in bound_list:
        nic_file_name = '/sys/bus/pci/devices/' + nic_port + '/numa_node'
        with open(nic_file_name, 'r') as nic_file_handle_r:
            socket_num = nic_file_handle_r.read()
        if '1' in socket_num:
            socket_1_list.append(nic_port)
        elif '0' in socket_num:
            socket_0_list.append(nic_port)
    for dma_port in dma_bound_list:
        dma_file_name = '/sys/bus/pci/devices/' + dma_port + '/numa_node'
        with open(dma_file_name, 'r') as dma_file_handle_r:
            dma_socket = dma_file_handle_r.read()
        if '1' in dma_socket:
            dma_socket_1_list.append(dma_port)
        elif '0' in dma_socket:
            dma_socket_0_list.append(dma_port)

    if len(socket_0_list) >= 2:
        return socket_0_list, dma_socket_0_list
    else:
        return socket_1_list, dma_socket_1_list


def bind_card(dpdk_path, build_path):
    not_bound_list = []
    bound_list = []
    dma_bound_list = []
    dma_not_bound_list = []
    check_cmd = dpdk_path + " -s"
    p1 = subprocess.Popen(check_cmd, stdout=subprocess.PIPE, shell=True)
    for line in p1.stdout.readlines():
        if "E810" in str(line):
            if "if=" in str(line):
                not_bound_list.append(line.decode("utf-8").split(" ")[0])
            else:
                bound_list.append(line.decode("utf-8").split(" ")[0])
            print(not_bound_list, bound_list)
        elif "ioatdma" in str(line):
            if "drv=vfio-pci unused=ioatdma" in str(line):
                dma_bound_list.append(line.decode("utf-8").split(" ")[0])
            else:
                dma_not_bound_list.append(line.decode("utf-8").split(" ")[0])

    if len(not_bound_list) > 0:
        print("++++++++++++++++++++++++NIC port is binding++++++++++++++++++++++++++++")
        for port in not_bound_list:
            bind_cmd = "sudo " + build_path + " bind_pmd " + port
            print(bind_cmd)
            p2 = subprocess.Popen(bind_cmd, stdout=subprocess.PIPE, shell=True)
            p2.communicate()
            bound_list.append(port)
    else:
        print("All NIC cards have been already binded")
    if len(dma_not_bound_list) > 0:
        print("++++++++++++++++++++++++DMA port is binding++++++++++++++++++++++++++++")
        for dma_port in dma_not_bound_list:
            dma_bind_cmd = "sudo " + dpdk_path + " -b vfio-pci " + dma_port
            print(dma_bind_cmd)
            p3 = subprocess.Popen(dma_bind_cmd, stdout=subprocess.PIPE, shell=True)
            p3.communicate()
            dma_bound_list.append(dma_port)
    else:
        print("All DMA have been already binded")

    nic_port_list, dma_port_list = check_socket(bound_list, dma_bound_list)
    return nic_port_list, dma_port_list


if __name__ == '__main__':
    dpdk_path_g = "/home/gta/IMTL/dpdk/usertools/dpdk-devbind.py"
    build_path_g = "/home/gta/IMTL/Media-Transport-Library/script/nicctl.sh"
    nic_port_list_g, dma_port_list_g = bind_card(dpdk_path_g, build_path_g)
    print(nic_port_list_g, dma_port_list_g)

