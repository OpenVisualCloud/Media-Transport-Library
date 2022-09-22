# obs-kahawai
obs source plguin for kahawai

## Build and Use
### build kahawai library
refer to: [build guide](../../doc/build.md)

### build & install obs-studio to /usr/local/
refer to: https://obsproject.com/wiki/build-instructions-for-linux

### build linux-kahawai
``` shell
cd linux-kahawai
meson build
meson complile -C build
sudo meson install -C build
```

### open obs-studio

``` shell
obs
```

### add kahawai input source

## TODO
### auto detect vfio-pci NIC ports
### auto detect NIC numa to provide usable lcores
### use st20 pipeline api
