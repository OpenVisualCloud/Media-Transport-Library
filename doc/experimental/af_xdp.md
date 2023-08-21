# AF_XDP Guide

## 1. Background

AF_XDP is an address family that is optimized for high-performance packet processing inside the kernel stack based on BPF and XDP. AF_XDP sockets enable XDP programs to redirect packets to a memory buffer in userspace. DPDK has built-in AF_XDP poll mode driver support. For details, please refer to <https://www.kernel.org/doc/HTML/latest/networking/af_xdp.html> and <https://doc.dpdk.org/guides/nics/af_xdp.html>.

By using AF_XDP, the Intel® Media Transport Library can leverage the full support of the Linux network stack. However, it's worth noting that there is a slight performance discrepancy compared to the full DPDK user PMD.

## 2. DPDK build with AF_XDP PMD

### 2.1 Install libxdp and libbpf

Get latest source code release from <https://github.com/libbpf/libbpf/releases> and <https://github.com/xdp-project/xdp-tools/releases>, and follow the instructions to build and install from source. Use “pkg-config --libs libxdp” and “pkg-config --libs libbpf” to check if the packages are correctly installed.

#### 2.2 Rebuild DPDK and and make sure af_xdp pmd driver is built

Check <https://github.com/DPDK/dpdk/blob/main/drivers/net/af_xdp/meson.build>.

## 3. Setup

Below is the command to assign an IP in case no DHCP support in your setup

```bash
sudo nmcli dev set enp175s0f0np0 managed no
sudo ifconfig enp175s0f0np0 192.168.108.101/24
sudo nmcli dev set enp175s0f1np1 managed no
sudo ifconfig enp175s0f1np1 192.168.108.102/24
```

Below is the tuned settings for performance.

```bash
echo 2 | sudo tee /sys/class/net/enp175s0f0np0/napi_defer_hard_irqs
echo 200000 | sudo tee /sys/class/net/enp175s0f0np0/gro_flush_timeout
echo 2 | sudo tee /sys/class/net/enp175s0f1np1/napi_defer_hard_irqs
echo 200000 | sudo tee /sys/class/net/enp175s0f1np1/gro_flush_timeout
```

You may need disable rp_filter for multicast report message.

```bash
sudo sysctl -w net.ipv4.conf.all.rp_filter=0
```

## 4. Run with root user

Refer to [afxdp config](../tests/script/afxdp_json/) for how to config the AF_XDP pmd in json config.

## 5. FAQs

### 5.1 No IP assigned

If you see below error while running RxTxApp with AFXDP json, please assign an IP for the port.

```bash
st_app_parse_json, using json-c version: 0.13.1
ST: st_socket_get_if_ip, SIOCGIFADDR fail -1 for if enp175s0f0np0
ST: st_user_params_check, get ip fail, if enp175s0f0np0
ST: st_init, st_user_params_check fail -1
main, st_init fail
```
