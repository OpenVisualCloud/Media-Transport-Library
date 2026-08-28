# Build guide for Windows

## Requirements

- Windows Server 2025

## Prepare build environment

1. Install MSYS2

    Download the latest installer from <https://www.msys2.org/>

1. Install npcap

    Download the latest installer from <https://npcap.com/#download>

1. Run MSYS2 UCRT64

    > **Note:** All the following commands should be executed in UCRT64 environment.

1. Install tools

    ```bash
    pacman -S git pactoys unzip
    ```

    ```bash
    pacboy -S dlfcn:p gcc:p gtest:p json-c:p libpcap:p meson:p mman-win32:p
    ```

1. Install npcap SDK

    ```bash
    wget https://npcap.com/dist/npcap-sdk-1.16.zip
    ```

    ```bash
    unzip -d npcap-sdk-1.16 ./npcap-sdk-1.16.zip
    ```

    ```bash
    cp -r ./npcap-sdk-1.16/lib/x64/. "${MSYSTEM_PREFIX}/lib"
    ```

## Build DPDK

1. Clone the MTL repository

    ```bash
    git clone https://github.com/OpenVisualCloud/Media-Transport-Library.git
    ```

    ```bash
    cd ./Media-Transport-Library
    ```

    ```bash
    MTL_PATH="$PWD"
    ```

1. Clone the DPDK repository

    > **Note:** The DPDK repository should be located directly in the MTL repository.

    `versions.env` in the MTL repository holds the DPDK version to use. Read the file to set `DPDK_VER`.

    ```bash
    . "$MTL_PATH"/versions.env
    ```

    ```bash
    git clone -b "v${DPDK_VER}" https://github.com/DPDK/dpdk.git
    ```

1. Convert the patch symlinks to files

    Run this step for the DPDK versions 22.03 to 23.11. For every other version, go to the
    next step.

    > **Note:** Some patch files in these versions point to a patch of an older DPDK version.
    > Git can write such a file as a text file that holds the target path. It does this when
    > the checkout has `core.symlinks=false`. A file system that cannot hold symlinks makes
    > Git set this value at clone time. This is frequent on Windows. Some of these files are
    > text in the repository itself. `git am` and `git apply` reject a text file.

    The command replaces each text file with the content of its target. A symlink can point
    to a second symlink. The longest chain in the repository is two hops. The command makes
    three passes, one more than the chain needs. It rewrites only a file whose first line
    starts with `../` and ends with `.patch`, so it is safe to run twice.

    ```bash
    for _ in 1 2 3; do
        for f in "$MTL_PATH"/patches/dpdk/"${DPDK_VER}"/*.patch \
                 "$MTL_PATH"/patches/dpdk/"${DPDK_VER}"/windows/*.patch; do
            [ -f "$f" ] || continue
            target=$(head -n 1 "$f")
            case "$target" in ../*.patch) cp "$(dirname "$f")/$target" "$f" ;; esac
        done
    done

    for f in "$MTL_PATH"/patches/dpdk/"${DPDK_VER}"/*.patch \
             "$MTL_PATH"/patches/dpdk/"${DPDK_VER}"/windows/*.patch; do
        [ -f "$f" ] || continue
        case "$(head -n 1 "$f")" in ../*.patch) echo "not converted: $f" ;; esac
    done
    ```

    The command then checks each file again. A line in the output means the conversion is not
    complete. Report such a file in a GitHub issue.

    > **Note:** The three passes are enough only while every chain stays inside one directory
    > depth. A chain such as `23.11/windows/A.patch -> ../../23.07/A.patch -> ../23.03/A.patch`
    > makes the second pass compute the path `23.11/windows/../23.03/A.patch`. This path does
    > not exist, so `cp` writes an error and the text file stays. Nothing in the repository
    > enforces the depth rule.

    Do not commit the result. A commit of the converted tree replaces each symlink with a
    large file. With `core.symlinks=false`, Git keeps the symlink mode and writes the patch
    body as the link target. Such an entry is not valid. No tool in this repository and no CI
    job finds either result.

    To remove the changes, use this command. It restores from `HEAD`, not from the index, so
    it also works after `git add`. It reverts the whole version directory:

    ```bash
    git -C "$MTL_PATH" restore --source=HEAD --staged --worktree patches/dpdk/"${DPDK_VER}"
    ```

1. Apply the MTL patches for DPDK

    ```bash
    cd "${MTL_PATH}/dpdk"
    ```

    ```bash
    git am "$MTL_PATH"/patches/dpdk/"${DPDK_VER}"/*.patch
    ```

    ```bash
    git apply "$MTL_PATH"/patches/dpdk/"${DPDK_VER}"/windows/*.patch
    ```

1. Build DPDK

    ```bash
    meson setup -Dmax_lcores=256 build
    ```

    ```bash
    meson compile -C build
    ```

    Create a copy of the `sched.h` file

    > **Note:** DPDK installation overwrites the `sched.h` file and cause MTL build problems

    ```bash
    cp "${MSYSTEM_PREFIX}/include/sched.h" "${MTL_PATH}/sched.h.bak"
    ```

    ```bash
    meson install -C build
    ```

    Restore the copy

    ```bash
    cp "${MTL_PATH}/sched.h.bak" "${MSYSTEM_PREFIX}/include/sched.h"
    ```

## Build MTL

1. Run the build script

    ```bash
    cd "$MTL_PATH"
    ```

    ```bash
    ./build.sh debugonly
    ```
