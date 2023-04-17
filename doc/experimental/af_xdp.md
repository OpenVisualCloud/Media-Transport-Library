# AF_XDP Guide

## 1. Background

AF_XDP is an address family that is optimized for high performance packet processing inside kernel stack based on BPF and XDP. AF_XDP sockets enable the possibility for XDP program to redirect packets to a memory buffer in userspace. DPDK has built-in AF_XDP poll mode driver support. For the details pls refer to <https://www.kernel.org/doc/HTML/latest/networking/af_xdp.html> and <https://doc.dpdk.org/guides/nics/af_xdp.html>.
With AF_XDP, Intel® Media Transport Library get the benefits with all linux network stack support, downsides is little performance gap compared to full DPDK user PMD.

## 2. DPDK build with AF_XDP PMD

### 2.1 Install libxdp and libbpf

Get latest source code release from <https://github.com/libbpf/libbpf/releases> and <https://github.com/xdp-project/xdp-tools/releases>, and follow the guide to build and install from source. Use “pkg-config --libs libxdp” and “pkg-config --libs libbpf” to check if the package are correctly installed.

#### 2.2 Rebuild DPDK and and make sure af_xdp pmd driver is built

Check <https://github.com/DPDK/dpdk/blob/main/drivers/net/af_xdp/meson.build>.

## 3. Setup

### 3.1 Update kernel(>5.10) and NIC driver to latest version((>=1.9.11) )

Double check the kernel version.

```bash
cat /proc/version
```

Double check the driver version is right from dmesg.

```bash
[22818.047860] ice: Intel(R) Ethernet Connection E800 Series Linux Driver - version 1.9.11
```

### 3.2 Update DDP package version(>1.3.30)

Double check the DDP version is right from dmesg.

```bash
The DDP package was successfully loaded: ICE OS Default Package (mc) version 1.3.30.0
```

Use below command to update if it's not latest.

```bash
cd /usr/lib/firmware/updates/intel/ice/ddp
cp <latest_ddp_dir>/ice-1.3.30.0.pkg ./
rm ice.pkg
ln -s ice-1.3.30.0.pkg ice.pkg
rmmod ice
modprobe ice
```

### 3.3 Assing IP and tuned settings

Below is the command to assign an IP in case no DHCP support in your setup

```bash
sudo nmcli dev set ens801f0 managed no
sudo ifconfig ens801f0 192.168.108.101/24
sudo nmcli dev set ens801f1 managed no
sudo ifconfig ens801f1 192.168.108.102/24
```

Below is the tuned settings for performance.

```bash
echo 2 | sudo tee /sys/class/net/ens801f0/napi_defer_hard_irqs
echo 200000 | sudo tee /sys/class/net/ens801f0/gro_flush_timeout
echo 2 | sudo tee /sys/class/net/ens801f1/napi_defer_hard_irqs
echo 200000 | sudo tee /sys/class/net/ens801f1/gro_flush_timeout
```

You may need disable rp_filter for multicast report message.

```bash
sudo sysctl -w net.ipv4.conf.all.rp_filter=0
```

## 4. Run with root user

Refer to [afxdp config](../tests/script/afxdp_json/) for how to config the AF_XDP pmd in json config.

## 5. FAQs

### 5.1 No IP assigned

If you see below error while running rxtxapp with AFXDP json, pls assign IP for the port.

```bash
st_app_parse_json, using json-c version: 0.13.1
ST: st_socket_get_if_ip, SIOCGIFADDR fail -1 for if ens801f0
ST: st_user_params_check, get ip fail, if ens801f0 for P port
ST: st_init, st_user_params_check fail -1
main, st_init fail
```
