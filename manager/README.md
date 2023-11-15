# MTL Manager

Manager is a daemon server running under root privilege. It is responsible for managing the instances of MTL and handling control plane configurations which require high privileges.

## Build

```bash
meson setup build
meson compile -C build
```

## Run

```bash
sudo ./build/MtlManager
```
