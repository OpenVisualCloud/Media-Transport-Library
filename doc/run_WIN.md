# Run guide for Windows

## Enable large pages

1. Run Local Security Policy

    1. Press `Win + R`

    1. Run `secpol.msc`

1. Open *Local Policies/User Rights Assignment/Lock pages in memory*

1. Add current user

## Disable driver signature enforcement

```
bcdedit.exe /set testsigning on
```

## Install drivers

1. Install WDK (Windows Driver Kit)

    1. Follow steps 1 - 2 of the *Install the WDK using WinGet* guide

        https://learn.microsoft.com/en-us/windows-hardware/drivers/install-the-wdk-using-winget

    1. Install Windows Driver Kit extenstion

        1. Run Visual Studio Installer

        1. Select *Modify*

        1. Open *Individual components* tab

        1. Select *Windows Driver Kit*

        1. Select *Modify*

1. Download driver source code

    ```
    git clone https://dpdk.org/git/dpdk-kmods
    ```

1. Build and install virt2phys and NetUIO drivers

    Refer to the README files (in the `dpdk-kmods/windows` directory).


## Setup NIC

1. Download the latest DDP package

    https://www.intel.com/content/www/us/en/download/19630/intel-network-adapter-driver-for-800-series-devices-under-linux.html

1. Extract the archive

1. Create `C:\dpdk\lib` directory

1. Move the `ice-*\ddp\ice-*.pkg` file to the `C:\dpdk\lib` directory

1. Rename the file to `ice.pkg` (the full path should be `C:\dpdk\lib\ice.pkg`)

1. Create `C:\temp` directory
