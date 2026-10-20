# obs-mtl

obs source plugin for Media Transport Library

## Build and Use

### build Media Transport Library library

refer to: [build guide](../../doc/build.md)

### build & install obs-studio

refer to: <https://obsproject.com/wiki/build-instructions-for-linux>

### build linux-mtl

Build and install the plugin with the script, from the top of the MTL source tree:

```bash
./script/build_obs_plugin.sh
```

Or build it step by step:

```bash
cd linux-mtl
meson setup build
meson compile -C build
sudo meson install -C build
```

### open obs-studio

```bash
obs
```

### add input source

## TODO

### output plugin   -   high

### auto detect vfio-pci NIC ports  -   middle

### auto detect NIC numa to provide usable lcores   -   low
