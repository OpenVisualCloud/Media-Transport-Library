#!/usr/bin/env python3
"""
MTL-MXL POC — VF / NIC Stats Collector → InfluxDB

Polls `ip -s link show <iface>` every POLL_INTERVAL seconds,
extracts per-VF and PF-level TX/RX byte counters, computes
deltas (bytes / sec → Gbps), and pushes to InfluxDB v2 using
the line protocol.

Measurement: nic_stats
Tags:        host=<hostname>, iface=<pf-name>, entity=<vfN|pf>, role=<label>
Fields:      tx_bytes=<u64>u, rx_bytes=<u64>u,
             tx_gbps=<f64>, rx_gbps=<f64>,
             tx_pps=<u64>u, rx_pps=<u64>u

Usage (sender — machine 58):
  source poc/monitoring/env.sh
  python3 poc/scripts/vf_stats_collector.py \
      --pf-iface eth0 \
      --vf-roles '0=synth_tx_p,1=mtl_rx_p,2=synth_tx_r,3=mtl_rx_r,4=poc14_tx_p,5=poc14_tx_r' \
      --extra-iface eth1 --extra-role rdma_tx

Usage (receiver — machine 51):
  source poc/monitoring/env.sh
  python3 poc/scripts/vf_stats_collector.py \
      --pf-iface eth2 \
      --vf-roles '0=rdma_rx_p,1=rdma_rx_r,2=poc14_vf2,3=poc14_vf3,4=poc14_vf4,5=poc14_vf5' \
      --extra-iface eth0 --extra-role st2110_rx
"""

import argparse
import os
import re
import socket
import subprocess
import sys
import time
import urllib.request
import urllib.error
from urllib.parse import urlparse


def _setup_no_proxy():
    """Ensure InfluxDB URL host bypasses HTTP proxy."""
    influx_url = os.environ.get('INFLUXDB_URL', '')
    if not influx_url:
        return
    host = urlparse(influx_url).hostname or ''
    if not host:
        return
    current = os.environ.get('no_proxy', os.environ.get('NO_PROXY', ''))
    hosts = [h.strip() for h in current.split(',') if h.strip()]
    if host not in hosts:
        hosts.append(host)
        val = ','.join(hosts)
        os.environ['no_proxy'] = val
        os.environ['NO_PROXY'] = val


_setup_no_proxy()


# ── Parse `ip -s link show <iface>` ──

def parse_ip_link_stats(iface: str) -> dict:
    """
    Parse PF + per-VF stats from `ip -s link show <iface>`.
    Returns {
       'pf': {'tx_bytes': int, 'rx_bytes': int, 'tx_pkts': int, 'rx_pkts': int},
       'vf0': {'tx_bytes': int, 'rx_bytes': int, 'tx_pkts': int, 'rx_pkts': int},
       ...
    }
    """
    try:
        out = subprocess.check_output(
            ['ip', '-s', 'link', 'show', iface],
            stderr=subprocess.STDOUT, timeout=5
        ).decode(errors='replace')
    except (subprocess.CalledProcessError, subprocess.TimeoutExpired, FileNotFoundError):
        return {}

    result = {}
    lines = out.splitlines()
    i = 0

    # ── PF-level stats ──
    while i < len(lines):
        line = lines[i].strip()
        # PF RX header:  "RX:  bytes  packets errors ..."
        if re.match(r'^RX:\s+bytes\s+packets', line) and 'vf' not in _context(lines, i):
            i += 1
            if i < len(lines):
                vals = lines[i].split()
                if len(vals) >= 2:
                    result.setdefault('pf', {})
                    result['pf']['rx_bytes'] = int(vals[0])
                    result['pf']['rx_pkts'] = int(vals[1])
            i += 1
            continue
        # PF TX header
        if re.match(r'^TX:\s+bytes\s+packets', line) and 'vf' not in _context(lines, i):
            i += 1
            if i < len(lines):
                vals = lines[i].split()
                if len(vals) >= 2:
                    result.setdefault('pf', {})
                    result['pf']['tx_bytes'] = int(vals[0])
                    result['pf']['tx_pkts'] = int(vals[1])
            i += 1
            continue
        # ── VF block: "vf N     link/ether ..." ──
        m = re.match(r'vf\s+(\d+)\s+', line)
        if m:
            vf_id = int(m.group(1))
            key = f'vf{vf_id}'
            result.setdefault(key, {'tx_bytes': 0, 'rx_bytes': 0, 'tx_pkts': 0, 'rx_pkts': 0})
            # Next lines have RX then TX
            i += 1
            # VF RX header
            while i < len(lines):
                vl = lines[i].strip()
                if re.match(r'^RX:\s+bytes\s+packets', vl):
                    i += 1
                    if i < len(lines):
                        vals = lines[i].split()
                        if len(vals) >= 2:
                            result[key]['rx_bytes'] = int(vals[0])
                            result[key]['rx_pkts'] = int(vals[1])
                    i += 1
                    break
                i += 1
            # VF TX header
            while i < len(lines):
                vl = lines[i].strip()
                if re.match(r'^TX:\s+bytes\s+packets', vl):
                    i += 1
                    if i < len(lines):
                        vals = lines[i].split()
                        if len(vals) >= 2:
                            result[key]['tx_bytes'] = int(vals[0])
                            result[key]['tx_pkts'] = int(vals[1])
                    i += 1
                    break
                # If we hit the next VF or EOF, break
                if re.match(r'vf\s+\d+\s+', vl):
                    break
                i += 1
            continue
        i += 1

    return result


def _context(lines, idx):
    """Return a few lines around idx for context detection."""
    start = max(0, idx - 3)
    return '\n'.join(lines[start:idx])


def parse_ethtool_stats(iface: str) -> dict:
    """Parse port-level counters via `ethtool -S <iface>`.

    Returns both MAC-level (.nic) and PF-VSI-level (no suffix) counters
    so the caller can compute RDMA = .nic − PF_VSI − VF_sum.
    """
    try:
        out = subprocess.check_output(
            ['ethtool', '-S', iface],
            stderr=subprocess.STDOUT, timeout=5
        ).decode(errors='replace')
    except (subprocess.CalledProcessError, subprocess.TimeoutExpired, FileNotFoundError):
        return {}

    result = {}
    for line in out.splitlines():
        line = line.strip()
        if line.startswith('rx_bytes.nic:'):
            result['nic_rx_bytes'] = int(line.split(':',1)[1].strip())
        elif line.startswith('tx_bytes.nic:'):
            result['nic_tx_bytes'] = int(line.split(':',1)[1].strip())
        elif line.startswith('rx_unicast.nic:'):
            result['nic_rx_pkts'] = int(line.split(':',1)[1].strip())
        elif line.startswith('tx_unicast.nic:'):
            result['nic_tx_pkts'] = int(line.split(':',1)[1].strip())
        elif line.startswith('rx_bytes:'):
            result['pf_rx_bytes'] = int(line.split(':',1)[1].strip())
        elif line.startswith('tx_bytes:'):
            result['pf_tx_bytes'] = int(line.split(':',1)[1].strip())
    return result


# ── InfluxDB push ──

def influx_push(lines_body: str, url: str, token: str, org: str, bucket: str):
    """POST line-protocol body to InfluxDB v2 /api/v2/write."""
    endpoint = f"{url}/api/v2/write?org={org}&bucket={bucket}&precision=ns"
    data = lines_body.encode('utf-8')
    req = urllib.request.Request(
        endpoint,
        data=data,
        method='POST',
        headers={
            'Authorization': f'Token {token}',
            'Content-Type': 'text/plain; charset=utf-8',
        }
    )
    try:
        with urllib.request.urlopen(req, timeout=3) as resp:
            _ = resp.read()
    except urllib.error.HTTPError as e:
        body = e.read().decode(errors='replace')[:200]
        print(f"[VF-STATS] InfluxDB push error {e.code}: {body}", file=sys.stderr)
    except Exception as e:
        print(f"[VF-STATS] InfluxDB push failed: {e}", file=sys.stderr)


# ── EWMA smoothing for noisy counters (e.g. PF-level stats on ice) ──

_ewma_state: dict = {}   # key = (iface, entity, field) → smoothed value
EWMA_ALPHA = 0.3          # weight of new sample; lower = smoother


def _ewma(iface: str, entity: str, field: str, raw: float) -> float:
    """Apply exponential weighted moving average to a rate value."""
    key = (iface, entity, field)
    prev = _ewma_state.get(key)
    if prev is None:
        _ewma_state[key] = raw
        return raw
    smoothed = EWMA_ALPHA * raw + (1 - EWMA_ALPHA) * prev
    _ewma_state[key] = smoothed
    return smoothed


# ── Build line-protocol body ──

def build_lines(host: str, iface: str, current: dict, prev: dict,
                dt: float, vf_roles: dict, ts_ns: int,
                smooth: bool = False) -> str:
    """Build InfluxDB line-protocol for all VFs + PF on an interface.

    If smooth=True, apply EWMA to gbps/pps values (useful for PF-level
    counters whose hardware updates are bursty).
    """
    lines = []
    for entity, cur in current.items():
        if entity not in prev:
            continue
        prv = prev[entity]
        dtx = cur.get('tx_bytes', 0) - prv.get('tx_bytes', 0)
        drx = cur.get('rx_bytes', 0) - prv.get('rx_bytes', 0)
        dtx_p = cur.get('tx_pkts', 0) - prv.get('tx_pkts', 0)
        drx_p = cur.get('rx_pkts', 0) - prv.get('rx_pkts', 0)
        # Skip if counters wrapped or reset
        if dtx < 0 or drx < 0:
            continue
        tx_gbps = (dtx * 8) / (dt * 1e9) if dt > 0 else 0.0
        rx_gbps = (drx * 8) / (dt * 1e9) if dt > 0 else 0.0
        tx_pps = int(dtx_p / dt) if dt > 0 else 0
        rx_pps = int(drx_p / dt) if dt > 0 else 0

        if smooth:
            tx_gbps = _ewma(iface, entity, 'tx_gbps', tx_gbps)
            rx_gbps = _ewma(iface, entity, 'rx_gbps', rx_gbps)

        role = vf_roles.get(entity, entity)
        line = (
            f'nic_stats,host={host},iface={iface},entity={entity},role={role} '
            f'tx_bytes={cur["tx_bytes"]}u,'
            f'rx_bytes={cur["rx_bytes"]}u,'
            f'tx_gbps={tx_gbps:.4f},'
            f'rx_gbps={rx_gbps:.4f},'
            f'tx_pps={tx_pps}u,'
            f'rx_pps={rx_pps}u '
            f'{ts_ns}'
        )
        lines.append(line)
    return '\n'.join(lines)


# ── Main loop ──

def main():
    parser = argparse.ArgumentParser(
        description='Collect VF/NIC stats and push to InfluxDB')
    parser.add_argument('--pf-iface', required=True,
                        help='PF interface with VFs (e.g. eth0)')
    parser.add_argument('--vf-roles', default='',
                        help='Comma-separated VF→role map: 0=synth_tx_p,1=mtl_rx_p,...')
    parser.add_argument('--extra-iface', default='',
                        help='Additional interface to monitor PF-level stats (e.g. RDMA iface)')
    parser.add_argument('--extra-role', default='rdma',
                        help='Role label for the extra interface (default: rdma)')
    parser.add_argument('--pf2-iface', default='',
                        help='Second PF interface with VFs (e.g. ens255np0 for poc_8k sender)')
    parser.add_argument('--pf2-vf-roles', default='',
                        help='VF→role map for PF2: 0=sender_8k_rx,...')
    parser.add_argument('--interval', type=float, default=2.0,
                        help='Poll interval in seconds (default: 2)')
    parser.add_argument('--quiet', action='store_true',
                        help='Suppress per-poll stdout output')
    args = parser.parse_args()

    # InfluxDB config from env
    url    = os.environ.get('INFLUXDB_URL', '')
    token  = os.environ.get('INFLUXDB_TOKEN', '')
    org    = os.environ.get('INFLUXDB_ORG', '')
    bucket = os.environ.get('INFLUXDB_BUCKET', '')
    if not all([url, token, org, bucket]):
        print("[VF-STATS] ERROR: INFLUXDB_URL / TOKEN / ORG / BUCKET env vars required",
              file=sys.stderr)
        print("           Hint: source poc/monitoring/env.sh", file=sys.stderr)
        sys.exit(1)

    host = socket.gethostname()

    # Parse VF role map
    vf_roles = {}
    if args.vf_roles:
        for pair in args.vf_roles.split(','):
            if '=' in pair:
                k, v = pair.split('=', 1)
                vf_roles[f'vf{k.strip()}'] = v.strip()
    vf_roles.setdefault('pf', f'{args.pf_iface}_pf')

    # Parse PF2 VF role map
    pf2_roles = {}
    if args.pf2_vf_roles:
        for pair in args.pf2_vf_roles.split(','):
            if '=' in pair:
                k, v = pair.split('=', 1)
                pf2_roles[f'vf{k.strip()}'] = v.strip()
    if args.pf2_iface:
        pf2_roles.setdefault('pf', f'{args.pf2_iface}_pf')

    print(f"[VF-STATS] Collecting stats every {args.interval}s", flush=True)
    print(f"[VF-STATS] PF iface: {args.pf_iface}  VF roles: {vf_roles}", flush=True)
    if args.pf2_iface:
        print(f"[VF-STATS] PF2 iface: {args.pf2_iface}  VF roles: {pf2_roles}", flush=True)
    if args.extra_iface:
        print(f"[VF-STATS] Extra iface: {args.extra_iface}  role={args.extra_role}", flush=True)
    print(f"[VF-STATS] InfluxDB: {url}  org={org}  bucket={bucket}", flush=True)

    prev_vf = {}
    prev_pf2 = {}
    prev_extra = {}
    prev_time = None

    while True:
        now = time.time()
        ts_ns = int(now * 1e9)

        # Collect VF stats from primary PF
        cur_vf = parse_ip_link_stats(args.pf_iface)

        # Collect VF stats from secondary PF (e.g. poc_8k sender)
        cur_pf2 = {}
        if args.pf2_iface:
            cur_pf2 = parse_ip_link_stats(args.pf2_iface)

        # Collect extra iface stats via ethtool -S
        cur_extra = {}
        if args.extra_iface:
            cur_extra = parse_ethtool_stats(args.extra_iface)

        dt = (now - prev_time) if prev_time else 0

        body_parts = []

        # VF lines
        if prev_vf and dt > 0:
            vf_body = build_lines(host, args.pf_iface, cur_vf, prev_vf,
                                  dt, vf_roles, ts_ns)
            if vf_body:
                body_parts.append(vf_body)

        # PF2 VF lines
        if prev_pf2 and dt > 0 and args.pf2_iface:
            pf2_body = build_lines(host, args.pf2_iface, cur_pf2, prev_pf2,
                                   dt, pf2_roles, ts_ns)
            if pf2_body:
                body_parts.append(pf2_body)

        # Extra iface: RDMA throughput from ethtool PF-VSI counters.
        # On ice + irdma, the PF VSI (ethtool tx_bytes/rx_bytes without
        # .nic suffix) carries RDMA traffic.  The .nic counters are the
        # whole port (PF VSI + VFs).  We use the PF VSI directly.
        rdma_tx_gbps = 0.0
        rdma_rx_gbps = 0.0
        if prev_extra and dt > 0 and cur_extra and prev_extra:
            pf_dtx = cur_extra.get('pf_tx_bytes', 0) - prev_extra.get('pf_tx_bytes', 0)
            pf_drx = cur_extra.get('pf_rx_bytes', 0) - prev_extra.get('pf_rx_bytes', 0)
            if pf_dtx < 0: pf_dtx = 0
            if pf_drx < 0: pf_drx = 0

            rdma_tx_gbps = (pf_dtx * 8) / (dt * 1e9) if dt > 0 else 0.0
            rdma_rx_gbps = (pf_drx * 8) / (dt * 1e9) if dt > 0 else 0.0

            role = args.extra_role
            line = (
                f'nic_stats,host={host},iface={args.extra_iface},entity=rdma,role={role} '
                f'tx_bytes={cur_extra.get("pf_tx_bytes",0)}u,'
                f'rx_bytes={cur_extra.get("pf_rx_bytes",0)}u,'
                f'tx_gbps={rdma_tx_gbps:.4f},'
                f'rx_gbps={rdma_rx_gbps:.4f},'
                f'tx_pps=0u,'
                f'rx_pps=0u '
                f'{ts_ns}'
            )
            body_parts.append(line)

        if body_parts:
            full_body = '\n'.join(body_parts)
            influx_push(full_body, url, token, org, bucket)

            if not args.quiet:
                # Print summary
                for entity, cur in cur_vf.items():
                    if entity in prev_vf and dt > 0:
                        prv = prev_vf[entity]
                        dtx = cur.get('tx_bytes', 0) - prv.get('tx_bytes', 0)
                        drx = cur.get('rx_bytes', 0) - prv.get('rx_bytes', 0)
                        tx_g = (dtx * 8) / (dt * 1e9)
                        rx_g = (drx * 8) / (dt * 1e9)
                        role = vf_roles.get(entity, entity)
                        print(f"  {entity:4s} ({role:14s})  TX {tx_g:7.3f} Gbps  RX {rx_g:7.3f} Gbps")
                if args.extra_iface and 'rdma_tx_gbps' in dir():
                    print(f"  rdma ({args.extra_role:14s})  TX {rdma_tx_gbps:7.3f} Gbps  RX {rdma_rx_gbps:7.3f} Gbps")
                print(flush=True)

        prev_vf = cur_vf
        prev_pf2 = cur_pf2
        prev_extra = cur_extra
        prev_time = now

        time.sleep(args.interval)


if __name__ == '__main__':
    main()
