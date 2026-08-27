#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
# Copyright(c) 2026 Intel Corporation

import argparse
import subprocess
import sys

import yaml

# Peak rate at which a single-host case writes its RX dump. The heaviest writer
# is test_st20p_resolutions, which parametrizes over every yuv_files_422rfc10
# entry: RFC4175 PG2BE10 is 2.5 B/px, so 8K p25 (7680x4320) is 2.074 GB/s and 4K
# p60 is 1.244 GB/s. Nothing bounds the dump for those -- RxTxApp's
# rx_max_file_size defaults to 0 and only one test sets a cap -- so the mount has
# to be sized for the full run.
_PEAK_RX_BYTES_PER_S = 2_100_000_000

# The media tmpfs holds that dump alongside the input asset copied onto it.
_MEDIA_ASSET_HEADROOM_GIB = 8

# Never leave the mount smaller than the fixed size this replaced, whatever
# test_time or the RAM clamp below say.
_MEDIA_RAMDISK_FLOOR_GIB = 16


def _usable_mem_gib() -> int:
    """RAM available to a tmpfs on the machine running this generator, in GiB.

    Returns 0 if ``/proc/meminfo`` is unreadable. ``MemTotal`` counts hugetlb,
    which the acceptance suite reserves heavily (24x1 GiB per NUMA node at
    session start, plus 2048x2 MiB from the setup script), so it is reduced by
    ``Hugetlb``. ``MemAvailable`` is not used: it moves with page cache, which
    would make the generated config depend on when it was generated.
    """
    fields = {}
    try:
        with open("/proc/meminfo") as meminfo:
            for line in meminfo:
                name, _, rest = line.partition(":")
                if name in ("MemTotal", "Hugetlb"):
                    fields[name] = int(rest.split()[0])
    except (OSError, ValueError, IndexError):
        return 0
    if "MemTotal" not in fields:
        return 0
    return max(0, fields["MemTotal"] - fields.get("Hugetlb", 0)) // (1 << 20)


def _media_ramdisk_gib(test_time: int) -> int:
    """Size cap for the media tmpfs, derived from the configured ``test_time``.

    Under-sizing this mount does not merely truncate an RX dump: filesink
    reports ENOSPC as a pipeline error, and the byte-throughput oracles read the
    shortfall as an MTL delivery failure, so a healthy run fails for want of
    disk. tmpfs allocates lazily, so a generous cap costs nothing until a test
    actually writes that much.

    Two limits are applied after the derived size, in this order: half of the
    non-hugetlb RAM, then :data:`_MEDIA_RAMDISK_FLOOR_GIB`. The floor wins, so
    on a small host the cap can still exceed half of RAM -- deliberately,
    because that is the fixed size this calculation replaced and lowering it
    would be a regression for small hosts. RAM is read on the machine running
    the generator, which in a dual-host topology is not necessarily the host
    being sized.
    """
    dump_gib = -(-_PEAK_RX_BYTES_PER_S * test_time // (1 << 30))  # ceiling
    want_gib = dump_gib + _MEDIA_ASSET_HEADROOM_GIB
    usable_gib = _usable_mem_gib()
    capped_gib = min(want_gib, usable_gib // 2) if usable_gib else want_gib
    if capped_gib < want_gib:
        print(
            f"warning: media ramdisk capped at {capped_gib} GiB (half of the "
            f"{usable_gib} GiB not reserved as hugepages) but test_time="
            f"{test_time} can write up to {dump_gib} GiB; the heaviest "
            f"tests/single cases may hit ENOSPC",
            file=sys.stderr,
        )
    return max(capped_gib, _MEDIA_RAMDISK_FLOOR_GIB)


def _bdf_to_vendor_device(pci_id: str) -> str:
    """Resolve a PCI BDF (e.g. '0000:c9:00.0') to 'vendor:device' (e.g.
    '8086:1592') via lspci. The framework's PCIDevice parser matches on
    vendor:device, not a bus address. Already-resolved vendor:device values
    (no '.' — a BDF always has one, e.g. domain:bus:dev.func) and BDFs
    lspci can't resolve (e.g. no real hardware present, used in tests) are
    returned unchanged.
    """
    if "." not in pci_id:
        return pci_id
    try:
        out = subprocess.run(
            ["lspci", "-s", pci_id.removeprefix("0000:"), "-n"],
            capture_output=True,
            text=True,
            timeout=5,
            check=True,
        ).stdout.strip()
        vendor_device = out.split()[2] if out else None
        return vendor_device or pci_id
    except Exception:
        return pci_id


def gen_test_config(
    session_id: int,
    mtl_path: str,
    pci_device: str,
    ebu_ip: str = None,
    ebu_user: str = None,
    ebu_password: str = None,
    media_path: str = "/mnt/media",
    test_time: int = 60,
    no_capture: bool = False,
    capture_pci_device: str = None,
    interface_type: str = None,
) -> str:
    pci_devices = [dev.strip() for dev in pci_device.split(",") if dev.strip()]

    test_config = {
        "session_id": session_id,
        "mtl_path": mtl_path,
        "media_path": media_path,
        "test_time": test_time,
        "ramdisk": {
            "media": {
                "mountpoint": "/mnt/ramdisk/media",
                "size_gib": _media_ramdisk_gib(test_time),
            },
            "tmpfs_size_gib": 8,
        },
    }

    # Cards without SR-IOV (i225/i226) have no VF to hand to DPDK, so their
    # tests bind the PF itself, and a single-port card cannot even do that --
    # it takes MTL's kernel-socket datapath (KERNEL). Tests read this through
    # test_config["interface_type"]; leaving it out keeps the VF default.
    if interface_type:
        test_config["interface_type"] = interface_type

    has_ebu = all([ebu_ip, ebu_user, ebu_password])
    # capture_pci_device is the preferred, unambiguous way to designate the
    # sniff NIC: it is a dedicated BDF, separate from --pci_device's DUT PF
    # list, so --pci_device can hold 1+ DUT PF candidates (e.g. two PFs on a
    # second card, needed so a PF-mode DUT test can find a candidate that
    # doesn't share an IOMMU group with the capture NIC) without disturbing
    # sniff-device selection. Falls back to the legacy "2nd comma-separated
    # --pci_device entry is the sniff device" behavior when not given.
    sniff_pci_device = capture_pci_device or (
        pci_devices[1] if len(pci_devices) >= 2 else None
    )
    has_sniff = bool(sniff_pci_device) and not no_capture
    test_config["compliance"] = has_ebu and has_sniff

    if test_config["compliance"]:
        test_config["ramdisk"]["pcap_dir"] = "/mnt/ramdisk/pcap"
        test_config["capture_cfg"] = {
            "enable": True,
            "pcap_dir": "/mnt/ramdisk/pcap",
            "sniff_pci_device": _bdf_to_vendor_device(sniff_pci_device),
        }
    else:
        # ``capture_cfg.enable: false`` has to be written out, not left
        # implicit: the pcap_capture fixture treats an ABSENT capture_cfg as
        # "this host can do compliance", so without it every test taking that
        # fixture fails with "ebu_server is not configured" instead of running
        # its data-path oracles. A host with no sniff NIC or no EBU
        # credentials cannot produce a verdict, and the generator is what knows
        # that, so it records the opt-out here.
        #
        # The branch is keyed on ``compliance`` (has_ebu AND has_sniff) rather
        # than on ``has_sniff`` alone for the same reason: a sniff NIC with no
        # EBU credentials can capture a pcap that nothing can then grade, and
        # arming the capture is what produced that failure.
        #
        # ``--no_capture`` reaches this branch too, via ``has_sniff``: it is the
        # operator saying the card has no port to spare -- a single-port i225,
        # or a second port needed for a redundant (ST2022-7) test.
        test_config["capture_cfg"] = {"enable": False}
    if has_ebu:
        test_config["ebu_server"] = {
            "ebu_ip": ebu_ip,
            "user": ebu_user,
            "password": ebu_password,
            "proxy": False,
        }

    return yaml.safe_dump(test_config, sort_keys=False)


def _make_host(
    name: str,
    role: str,
    pci_device: str,
    ip_address: str,
    username: str,
    password: str,
    key_path: str,
    extra_info: dict = None,
    capture_pci_device: str = None,
) -> dict:
    pci_devices = [dev.strip() for dev in pci_device.split(",")]
    # A dedicated capture_pci_device is appended as one more network_interfaces
    # entry (after all DUT PF candidates), rather than requiring the caller to
    # smuggle it into the comma-separated DUT list at a fixed index.
    if capture_pci_device and capture_pci_device not in pci_devices:
        pci_devices = pci_devices + [capture_pci_device]
    # interface_index is scoped PER pci_device (vendor:device) group, not a
    # flat position in this list — the framework resolves each declared
    # network_interfaces entry by filtering the real host's NICs down to
    # those matching pci_device, then indexing into *that* filtered list.
    # Two PFs of the same card model (e.g. both ports of one E830) must get
    # indices 0 and 1 within their shared 'vendor:device' group, even if a
    # differently-modeled capture NIC comes between them in this list.
    group_counts: dict = {}
    network_interfaces = []
    for pci_dev in pci_devices:
        vendor_device = _bdf_to_vendor_device(pci_dev)
        idx = group_counts.get(vendor_device, 0)
        network_interfaces.append({"pci_device": vendor_device, "interface_index": idx})
        group_counts[vendor_device] = idx + 1
    connection_options = {"port": 22, "username": username}
    connection_options["password"] = password if password else ""
    if key_path:
        connection_options["key_path"] = key_path
    host = {
        "name": name,
        "instantiate": True,
        "role": role,
        "network_interfaces": network_interfaces,
        "connections": [
            {
                "ip_address": ip_address,
                "connection_type": "SSHConnection",
                "connection_options": connection_options,
            }
        ],
    }
    if extra_info:
        host["extra_info"] = extra_info
    return host


def gen_topology_config(
    pci_devices: list[str],
    ip_addresses: list[str],
    usernames: list[str],
    passwords: list[str],
    key_paths: list[str],
    mtl_paths: list[str],
    media_path: str = "/mnt/media",
    capture_pci_device: str = None,
) -> str:
    n = len(ip_addresses)
    hosts = []
    for i in range(n):
        is_sut = i == n - 1
        extra_info = {"mtl_path": mtl_paths[i], "media_path": media_path}
        hosts.append(
            _make_host(
                name="host" if is_sut else f"host_{i}",
                role="sut" if is_sut else "client",
                pci_device=pci_devices[i],
                ip_address=ip_addresses[i],
                username=usernames[i],
                password=passwords[i],
                key_path=key_paths[i],
                extra_info=extra_info,
                # The dedicated capture NIC is only ever attached to the
                # SUT host — a client host in dual-host topologies has no
                # use for it.
                capture_pci_device=capture_pci_device if is_sut else None,
            )
        )

    topology_config = {
        "metadata": {"version": "2.4"},
        "hosts": hosts,
    }
    return yaml.safe_dump(topology_config, explicit_start=True, sort_keys=False)


def _extend_list(lst: list, n: int) -> list:
    """Extend a list to length n by repeating the last element."""
    return (lst + [lst[-1]] * (n - len(lst)))[:n]


def main() -> None:
    parser = argparse.ArgumentParser(
        description="Generate test and topology configs for the test framework."
    )
    parser.add_argument(
        "--session_id",
        type=int,
        choices=range(0, 256),
        required=True,
        help="session ID (0-255)",
    )
    parser.add_argument(
        "--mtl_path",
        type=str,
        nargs="+",
        required=True,
        help="MTL path(s) — one per host",
    )
    parser.add_argument(
        "--pci_device",
        type=str,
        nargs="+",
        required=True,
        help="PCI BDF(s) per host (comma-separated within each host)",
    )
    parser.add_argument(
        "--ip_address",
        type=str,
        nargs="+",
        required=True,
        help="IP address(es) — one per host",
    )
    parser.add_argument(
        "--username",
        type=str,
        nargs="+",
        required=True,
        help="SSH username(s) — one per host",
    )
    parser.add_argument("--password", type=str, nargs="+", default=[None])
    parser.add_argument("--key_path", type=str, nargs="+", default=[None])
    # Optional EBU args
    parser.add_argument("--ebu_ip", type=str, default=None)
    parser.add_argument("--ebu_user", type=str, default=None)
    parser.add_argument("--ebu_password", type=str, default=None)
    # Optional test settings
    parser.add_argument("--test_time", type=int, default=60)
    parser.add_argument("--media_path", type=str, default="/mnt/media")
    parser.add_argument(
        "--no_capture",
        action="store_true",
        help="Disable packet capture so the 2nd NIC port is available for redundant (ST2022-7) tests",
    )
    parser.add_argument(
        "--interface_type",
        type=str,
        default=None,
        choices=["VF", "PF", "KERNEL"],
        help=(
            "How the tests attach to the NIC: VF (default, SR-IOV cards), "
            "PF (cards without SR-IOV, e.g. a two-port i225/i226) or KERNEL "
            "(MTL's kernel-socket datapath, the only one a single-port card "
            "can serve)"
        ),
    )
    parser.add_argument(
        "--capture_pci_device",
        type=str,
        default=None,
        help=(
            "Dedicated capture NIC PF BDF for the SUT host, kept separate "
            "from --pci_device's DUT PF candidate(s). Lets --pci_device hold "
            "one or more DUT PF candidates (e.g. two PFs on a second "
            "physical card) without disturbing which NIC is used for "
            "netsniff-ng capture. Falls back to the legacy behavior (2nd "
            "comma-separated --pci_device entry is the sniff device) if "
            "omitted."
        ),
    )

    args = parser.parse_args()

    n = len(args.ip_address)
    passwords = _extend_list(args.password, n)
    key_paths = _extend_list(args.key_path, n)
    mtl_paths = _extend_list(args.mtl_path, n)
    pci_devices = _extend_list(args.pci_device, n)
    usernames = _extend_list(args.username, n)

    if not any(passwords) and not any(key_paths):
        parser.error("at least one of --password or --key_path is required")

    test_config_yaml = gen_test_config(
        session_id=args.session_id,
        mtl_path=mtl_paths[-1],
        pci_device=pci_devices[-1],
        ebu_ip=args.ebu_ip,
        ebu_user=args.ebu_user,
        ebu_password=args.ebu_password,
        media_path=args.media_path,
        test_time=args.test_time,
        no_capture=args.no_capture,
        capture_pci_device=args.capture_pci_device,
        interface_type=args.interface_type,
    )

    with open("test_config.yaml", "w") as file:
        file.write(test_config_yaml)
    with open("topology_config.yaml", "w") as file:
        file.write(
            gen_topology_config(
                pci_devices=pci_devices,
                ip_addresses=args.ip_address,
                usernames=usernames,
                passwords=passwords,
                key_paths=key_paths,
                mtl_paths=mtl_paths,
                media_path=args.media_path,
                capture_pci_device=args.capture_pci_device,
            )
        )


if __name__ == "__main__":
    main()
