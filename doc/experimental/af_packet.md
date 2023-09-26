# AF_PACKET Guide

## 1. Background

The AF_PACKET socket in Linux allows an application to receive and send raw packets. The DPDK AF_PACKET PMD binds to an AF_PACKET socket and allows a DPDK application to send and receive raw packets through the Kernel. Detail please refer to <https://doc.dpdk.org/guides/nics/af_packet.html>.

This option provides an opportunity for you to experiment with this library, even if your Network Interface Card (NIC) is not supported by DPDK's native Poll Mode Driver (PMD). Please be aware, however, that the performance may not be optimal as it relies on kernel sockets for packet handling.

## 2. Run AF_PACKET PMD with root user

AF_PACKET PMD rely on kernel AF_PACKET support which need root access, and please refer to [dpdk_af_packet config](../../tests/script/dpdk_af_packet_json/) for how to config the AF_PACKET pmd in json config.

Customize the kernel network interface name `enp175s0f0np0` as your setup

```bash
    "interfaces": [
        {
            "name": "af_packet:enp175s0f0np0",
            "rx_queues_cnt": "1",
        },
        {
            "name": "af_packet:enp175s0f1np1",
            "rx_queues_cnt": "1",
        }
    ],
```

## 3. FAQs

### 3.1 No IP assigned

If you see below error while running RxTxApp with AF_PACKET json, please assign an IP for the port.

```bash
st_app_parse_json, using json-c version: 0.13.1
ST: st_socket_get_if_ip, SIOCGIFADDR fail -1 for if enp175s0f0np0
ST: st_user_params_check, get ip fail, if enp175s0f0np0
ST: st_init, st_user_params_check fail -1
main, st_init fail
```
