# Sample code for UDP api.

## 1. Introduction:
The dir incldue the simple sample code for how to develop application quickly based on UDP api of media transport library.

## 2. Samples with POSIX socket compatible(file descriptor) API:
[ufd_client_sample.c](ufd_client_sample.c): A udp client(tx) application based on POSIX socket compatible(file descriptor) UDP API.<br>
[ufd_client.json](ufd_client.json): The sample configuration file for the NIC port setup which used by the lib, include PCIE port, ip, netmask, gateway and other info.<br>
[ufd_server_sample.c](ufd_server_sample.c): A udp client(tx) application based on POSIX socket compatible(file descriptor) UDP API.<br>
[ufd_server.json](ufd_server.json): The sample configuration file for server sample.<br>

#### 2.1 Run
Customize the p_tx_ip args and the ufd_client.json/ufd_server.json as your setup
```bash
MUFD_CFG=app/udp/ufd_client.json ./build/app/UfdClientSample --p_tx_ip 192.168.85.80
MUFD_CFG=app/udp/ufd_server.json ./build/app/UfdServerSample
```
