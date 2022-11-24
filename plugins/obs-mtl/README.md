# obs-mtl
obs source plguin for MTL

## Build and Use
### build MTL library
refer to: [build guide](../../doc/build.md)

### build & install obs-studio to /usr/local/
refer to: https://obsproject.com/wiki/build-instructions-for-linux

### build linux-mtl
``` shell
cd linux-mtl
meson build
meson complile -C build
sudo meson install -C build
```

### open obs-studio

``` shell
obs
```

### add MTL input source

## TODO
### auto detect vfio-pci NIC ports
### auto detect NIC numa to provide usable lcores
