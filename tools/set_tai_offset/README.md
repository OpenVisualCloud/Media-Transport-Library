# set_tai_offset

Sets the kernel TAI offset by parsing `/usr/share/zoneinfo/leap-seconds.list`.

ST 2110 uses PTP/TAI time. The Linux kernel needs to know the UTC-TAI offset
(currently 37 seconds) for `CLOCK_TAI` to return the correct value. This tool
automates what `adjtimex --tai 37` does, but reads the offset from the system's
leap-seconds file instead of hardcoding it.

## Build

```bash
meson setup build && meson compile -C build
```

## Usage

```bash
sudo ./build/set_tai_offset       # set if needed
sudo ./build/set_tai_offset -v    # verbose
```

If the offset is already set (> 0), it does nothing.

## What it does

1. Reads kernel TAI offset via `adjtimex()`
2. If already > 0, exits (already correct)
3. Parses the last offset from `/usr/share/zoneinfo/leap-seconds.list`
4. Sets it via `adjtimex(ADJ_TAI)`
5. Verifies the write

## Permissions

Requires `CAP_SYS_TIME` or root:

```bash
sudo setcap 'cap_sys_time+ep' ./build/set_tai_offset
```
