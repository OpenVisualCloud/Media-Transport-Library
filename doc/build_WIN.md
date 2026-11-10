# Build guide for Windows

This is the default way to build MTL on Windows: MSYS2 UCRT64, the DPDK
version of `versions.env`, and the MTL patches.

Tested on Windows 11 Pro (build 26300) with MinGW GCC 16.2 and DPDK 26.07.

- To prepare Windows and run MTL, see the [run guide](run_WIN.md).
- For a Windows VM with E810 or E830 VFs, see the [VM guide](vm_WIN.md).

## 1. Install MSYS2

1. In PowerShell:

    ```powershell
    $ProgressPreference = 'SilentlyContinue'
    Invoke-WebRequest https://repo.msys2.org/distrib/msys2-x86_64-latest.sfx.exe -OutFile msys2.sfx.exe
    .\msys2.sfx.exe -y -oC:\
    ```

1. Open the UCRT64 shell (`C:\msys64\ucrt64.exe`). Do all the next steps in
   this shell.

    On the first start, MSYS2 refreshes its keys. With no direct internet
    access this waits for minutes. Push Ctrl+C: the keys are already there.

1. Behind a proxy, set it for MSYS2. MSYS2 does not use the proxy settings of
   Windows:

    ```bash
    echo 'export http_proxy=http://<proxy>:<port> https_proxy=http://<proxy>:<port>' > /etc/profile.d/proxy.sh
    . /etc/profile.d/proxy.sh
    ```

1. Update MSYS2. The first update can close the shell. Then open it again and
   run the update again:

    ```bash
    pacman -Syuu
    ```

1. Install the tools:

    ```bash
    pacman -S --needed git pactoys unzip
    pacboy -S --needed dlfcn:p gcc:p gtest:p json-c:p libpcap:p meson:p \
        mman-win32:p ninja:p pkgconf:p python-pyelftools:p
    ```

1. Install the npcap SDK:

    ```bash
    curl -LO https://npcap.com/dist/npcap-sdk-1.16.zip
    unzip -d npcap-sdk-1.16 npcap-sdk-1.16.zip
    cp -r npcap-sdk-1.16/lib/x64/. "$MSYSTEM_PREFIX/lib"
    ```

## 2. Build DPDK

MTL reads the DPDK source in `<MTL>/dpdk`. `git am` makes commits, so git
needs a name and an email address.

```bash
mkdir -p /c/mtl && cd /c/mtl
git clone https://github.com/OpenVisualCloud/Media-Transport-Library.git
cd Media-Transport-Library
MTL_PATH="$PWD"
. ./versions.env
git clone -b "v${DPDK_VER}" https://github.com/DPDK/dpdk.git

git config --global user.name "<name>"
git config --global user.email "<email>"
cd dpdk
git am "$MTL_PATH"/patches/dpdk/"${DPDK_VER}"/*.patch
git am "$MTL_PATH"/patches/dpdk/"${DPDK_VER}"/windows/*.patch

meson setup -Dmax_lcores=256 build
meson compile -C build
meson install -C build
```

The MTL clone warns that `RxTxApp.py` and `rxtxapp.py` collide. NTFS does
not tell upper case from lower case. The build does not use these files.

The Windows patches of DPDK 26.07:

| Patch | What it does |
| --- | --- |
| 0001 | MinGW build and run fixes, `ice.pkg` from `C:\dpdk\lib`, the timer resolution |
| 0002 | Builds the static libraries only. MinGW GCC 15 and later cannot share `rte_errno` between DLLs. |
| 0003 | Does not install `sched.h` over the `sched.h` of MinGW |
| 0004 | Fixes a crash in `rte_eal_alarm_cancel()` |
| 0005 | Lets the iavf VF read its admin queue on Windows. Without it, each VF command times out. |

## 3. Build MTL

```bash
cd "$MTL_PATH"
./build.sh release
```

`libmtl.dll` needs `wpcap.dll`, the runtime of npcap. Install npcap, or, if
you do not capture packets, copy the libpcap of MSYS2:

```bash
cp "$MSYSTEM_PREFIX/bin/libpcap.dll" "$MSYSTEM_PREFIX/bin/wpcap.dll"
```

To run the unit tests (no NIC needed):

```bash
./build.sh unit
```
