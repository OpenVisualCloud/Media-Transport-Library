#!/usr/bin/env bash
# Power and frequency configuration for a worker.
#
# Five settings decide how much frequency an encoder core actually gets, and none
# of them shows up in the result as anything but a lower stream count:
#
#   P-state driver   intel_pstate=active   HWP, or passive for acpi-style control
#   scaling governor performance           never ramp down between frames
#   EPB (PerfSpect-selected source) 0      BIOS/OS hint, 0 = best performance
#   EPP (MSR 0x774)  0                     HWP hint, 0 = best performance
#   ELC              latency               Efficiency Latency Control [SRF+ only]
#
# scripts/check-bios.sh only *reports* these. This script sets them, and keeps
# them set across a reboot: PerfSpect writes MSRs and sysfs, and a reboot returns
# every one of them to the BIOS default.
#
# Run FROM THE CONTROLLER:
#   scripts/configure-power.sh [NODE]           # default: LAB_DEFAULT_NODE
#   scripts/configure-power.sh --verify [NODE]  # read back only, change nothing
#
# The values come from the "power and frequency" block of config/lab.env. Each
# node's sudo password is typed into that node's own prompt.
#
# Changing the P-state driver perturbs every CPU frequency on the node for a
# moment, so do not run this during a measurement.
set -Eeuo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
# shellcheck source=/dev/null
source "$ROOT/config/lab.env"
# shellcheck source=/dev/null
source "$ROOT/config/nodes.env"
: "${LAB_SSH_USER:?set LAB_SSH_USER in config/nodes.env}"
# shellcheck source=lib/remote-admin.sh
source "$ROOT/scripts/lib/remote-admin.sh"
lab_remote_admin_init "$LAB_SSH_USER"
# shellcheck source=lib/perfspect.sh
source "$ROOT/scripts/lib/perfspect.sh"

VERIFY_ONLY=0
if [[ "${1:-}" == "--verify" ]]; then VERIFY_ONLY=1; shift; fi
NODE="${1:-$LAB_DEFAULT_NODE}"
KEY="${NODE^^}_HOST"; KEY="${KEY//-/_}"
HOST="${!KEY:-}"
[[ -n "$HOST" ]] || { echo "FATAL: no address for $NODE ($KEY) in config/nodes.env" >&2; exit 2; }
TARGET="${LAB_SSH_USER}@$HOST"

GOVERNOR="${LAB_POWER_GOVERNOR:-performance}"
PSTATE="${LAB_POWER_PSTATE_DRIVER:-active}"
EPB="${LAB_POWER_EPB:-0}"
EPP="${LAB_POWER_EPP:-0}"
ELC="${LAB_POWER_ELC:-latency}"

# Validate here, not on the worker: a typo in lab.env should cost a second, not a
# half-applied profile on a remote machine.
case "$GOVERNOR" in performance|powersave) ;; *)
  echo "FATAL: LAB_POWER_GOVERNOR must be performance or powersave, not '$GOVERNOR'" >&2; exit 2 ;; esac
case "$PSTATE" in active|passive|off|skip) ;; *)
  echo "FATAL: LAB_POWER_PSTATE_DRIVER must be active, passive, off or skip, not '$PSTATE'" >&2; exit 2 ;; esac
case "$ELC" in latency|power|skip) ;; *)
  echo "FATAL: LAB_POWER_ELC must be latency, power or skip, not '$ELC'" >&2; exit 2 ;; esac
[[ "$EPB" =~ ^([0-9]|1[0-5])$ ]] ||
  { echo "FATAL: LAB_POWER_EPB must be 0-15 (0 = best performance), not '$EPB'" >&2; exit 2; }
[[ "$EPP" =~ ^([0-9]{1,2}|1[0-9]{2}|2[0-4][0-9]|25[0-5])$ ]] ||
  { echo "FATAL: LAB_POWER_EPP must be 0-255 (0 = best performance), not '$EPP'" >&2; exit 2; }

# 'set -e' on its own exits without a word, so the last thing on screen is
# whatever the failing remote command printed - which does not say which stage
# gave up, whether the node is half-configured, or how to undo it. Say all three.
on_error() {
  # The source file as well as the line: some of these commands run inside
  # scripts/lib/*.sh, where a bare line number points at the wrong file.
  local rc=$? line="$1" src="${2:-$0}"
  echo >&2
  echo "FATAL: configure-power.sh stopped at ${src##*/}:$line (exit $rc), in the" >&2
  echo "       stage named on the last '==' line above, which is the reason." >&2
  echo "       Re-running is safe: every stage here is idempotent." >&2
  echo "       If stage 2 got as far as recording, the pre-MXL configuration is on" >&2
  echo "       $NODE and this puts it back:" >&2
  echo "         cd ~/$LAB_PERFSPECT_DIR && sudo ./perfspect config restore \\" >&2
  echo "           ~/$LAB_PERFSPECT_RECORD_DIR/*_config.txt" >&2
  exit "$rc"
}
trap 'on_error $LINENO "${BASH_SOURCE[0]}"' ERR

# Read the state back from PerfSpect's config report for the controls it owns
# (governor/EPB/EPP/ELC), and from sysfs for the kernel-side pstate checks.
# Returns non-zero if required settings read back wrong.
VERIFY_EPP_WARNING=0
verify() {
  VERIFY_EPP_WARNING=0
  echo "== live power configuration on $NODE (PerfSpect + sysfs) =="
  if "${LAB_REMOTE_SSH[@]}" "$TARGET" "
    rc=0
    epp_warn=0
    ok()   { printf '  OK    %-24s %s\n' \"\$1\" \"\$2\"; }
    bad()  { printf '  WRONG %-24s %s\n' \"\$1\" \"\$2\"; rc=1; }
    warn() { printf '  WARN  %-24s %s\n' \"\$1\" \"\$2\"; }
    info() { printf '  info  %-24s %s\n' \"\$1\" \"\$2\"; }
    cfg_value() {
      key=\"\$1\"
      line=\$(printf '%s\n' \"\$perfspect_out\" | grep -F -m1 \"\$key:\" || true)
      [ -n \"\$line\" ] || return 1
      value=\${line#*:}
      printf '%s\n' \"\$value\" | sed -E 's/^[[:space:]]+//; s/[[:space:]]+--.*\$//; s/[[:space:]]+\$//'
    }
    cfg_num() {
      printf '%s\n' \"\$1\" | sed -nE 's/.*\\(([0-9]+)\\)\$/\\1/p'
    }

    driver=\$(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_driver 2>/dev/null || echo none)
    status=\$(cat /sys/devices/system/cpu/intel_pstate/status 2>/dev/null || echo absent)
    if [ '$PSTATE' = skip ]; then warn 'P-state driver' \"\$driver, status=\$status (not managed)\"
    elif [ \"\$status\" = '$PSTATE' ]; then ok 'P-state driver' \"\$driver, status=\$status\"
    elif [ \"\$status\" = absent ]; then warn 'P-state driver' \"\$driver (no intel_pstate on this kernel)\"
    else bad 'P-state driver' \"status=\$status, expected $PSTATE\"; fi

    perfspect_out=\$(cd ~/$LAB_PERFSPECT_DIR && $LAB_REMOTE_SUDO ./perfspect config --noupdate 2>/dev/null || true)
    if [ -z \"\$perfspect_out\" ]; then
      bad 'PerfSpect config' 'unable to read power settings (verify PerfSpect install and sudo access)'
    else
      gov=\$(cfg_value 'Scaling Governor' || true)
      if [ -z \"\$gov\" ]; then bad 'scaling governor' 'not reported by PerfSpect'
      elif [ \"\$gov\" = '$GOVERNOR' ]; then ok 'scaling governor' '$GOVERNOR (PerfSpect)'
      else bad 'scaling governor' \"\$gov - expected $GOVERNOR\"; fi

      epb=\$(cfg_value 'Energy Performance Bias' || true)
      epb_num=\$(cfg_num \"\$epb\")
      if [ -z \"\$epb\" ]; then bad 'energy_perf_bias' 'not reported by PerfSpect'
      elif [ -z \"\$epb_num\" ]; then bad 'energy_perf_bias' \"unreadable PerfSpect value: \$epb\"
      elif [ \"\$epb_num\" = '$EPB' ]; then ok 'energy_perf_bias' \"\$epb (PerfSpect-selected source)\"
      else bad 'energy_perf_bias' \"\$epb - expected $EPB\"; fi

      epp=\$(cfg_value 'Energy Performance Preference' || true)
      epp_num=\$(cfg_num \"\$epp\")
      if [ -z \"\$epp\" ]; then
        warn 'energy_perf_preference' 'not reported by PerfSpect (unsupported/unreadable on this platform)'
        epp_warn=1
      elif [ -z \"\$epp_num\" ]; then
        warn 'energy_perf_preference' \"unreadable PerfSpect value: \$epp (requested $EPP)\"
        epp_warn=1
      elif [ \"\$epp_num\" = '$EPP' ]; then
        info 'energy_perf_preference' \"\$epp (requested $EPP)\"
      else
        warn 'energy_perf_preference' \"\$epp (requested $EPP)\"
        epp_warn=1
      fi

      if [ '$ELC' = skip ]; then
        warn 'efficiency latency ctl' 'LAB_POWER_ELC=skip (not managed)'
      else
        elc=\$(cfg_value 'Efficiency Latency Control' || true)
        if [ -z \"\$elc\" ]; then
          warn 'efficiency latency ctl' 'not reported by PerfSpect (unsupported on this CPU is expected pre-SRF)'
        elif [ '$ELC' = latency ] && printf '%s' \"\$elc\" | grep -q 'Latency Optimized Mode'; then
          ok 'efficiency latency ctl' \"\$elc\"
        elif [ '$ELC' = power ] && printf '%s' \"\$elc\" | grep -q 'Optimized Power Mode'; then
          ok 'efficiency latency ctl' \"\$elc\"
        else
          bad 'efficiency latency ctl' \"\$elc - expected $ELC\"
        fi
      fi
    fi

    info 'no_turbo' \"\$(cat /sys/devices/system/cpu/intel_pstate/no_turbo 2>/dev/null || echo n/a)\"
    info 'kernel intel_pstate arg' \"\$(tr ' ' '\n' < /proc/cmdline | grep '^intel_pstate=' || echo '(none)')\"
    if systemctl is-enabled --quiet mxl-power-profile.service 2>/dev/null; then
      ok 'survives a reboot' 'mxl-power-profile.service is enabled'
    else
      warn 'survives a reboot' 'mxl-power-profile.service not enabled - EPB/EPP/ELC reset on reboot'
    fi
    if [ \"\$rc\" -ne 0 ]; then exit \"\$rc\"; fi
    if [ \"\$epp_warn\" -ne 0 ]; then exit 10; fi
    exit 0
  "; then
    return 0
  fi
  rc=$?
  if [[ "$rc" -eq 10 ]]; then
    VERIFY_EPP_WARNING=1
    return 0
  fi
  return "$rc"
}

if [[ "$VERIFY_ONLY" -eq 1 ]]; then
  echo "== ensuring PerfSpect is available on $NODE for verification =="
  lab_perfspect_install "$TARGET"
  if verify; then
    echo
    if [[ "$VERIFY_EPP_WARNING" -eq 1 ]]; then
      echo "Power configuration on $NODE matches required settings, with EPP warnings shown above."
    else
      echo "Power configuration on $NODE matches config/lab.env."
    fi
    exit 0
  fi
  echo
  echo "Power configuration on $NODE does not match config/lab.env." >&2
  echo "Apply it with: scripts/configure-power.sh $NODE" >&2
  exit 1
fi

echo "== plan =="
echo "  node:      $NODE ($HOST)"
echo "  pstate:    $PSTATE      governor: $GOVERNOR      EPB: $EPB      EPP: $EPP      ELC: $ELC"
echo "  login:     $LAB_SSH_USER${LAB_REMOTE_SUDO:+ (sudo password prompted on the node)}"

echo
echo "== 1/5 PerfSpect on $NODE =="
lab_perfspect_install "$TARGET"

echo
echo "== 2/5 recording the current configuration =="
# 'perfspect config' has no undo, and neither has a wrongly written MSR: --record
# is the only way back. Record ONCE and keep it - a second --record after this
# script has run would capture the modified state and restore to nowhere useful.
[[ -z "$LAB_REMOTE_SUDO" ]] || echo "-- enter the sudo password for $NODE when prompted"
"${LAB_REMOTE_SSH[@]}" "$TARGET" "
  set -e
  mkdir -p ~/$LAB_PERFSPECT_RECORD_DIR
  if ls ~/$LAB_PERFSPECT_RECORD_DIR/*_config.txt >/dev/null 2>&1; then
    echo '  keeping the existing record - it is the pre-MXL state, this run is not'
  else
    cd ~/$LAB_PERFSPECT_DIR
    $LAB_REMOTE_SUDO ./perfspect config --record --no-summary --noupdate \
      --output ~/$LAB_PERFSPECT_RECORD_DIR
    $LAB_REMOTE_SUDO chown -R \$(id -u):\$(id -g) ~/$LAB_PERFSPECT_RECORD_DIR
  fi
  ls -1 ~/$LAB_PERFSPECT_RECORD_DIR/*_config.txt | sed 's/^/  /'
"

echo
echo "== 3/5 P-state driver =="
if [[ "$PSTATE" == "skip" ]]; then
  echo "-- LAB_POWER_PSTATE_DRIVER=skip: leaving the driver alone"
else
  # Two halves, because they fail differently. /sys/.../intel_pstate/status
  # switches the driver on the running kernel; the GRUB drop-in is what makes the
  # choice survive a reboot. Doing only the first is how a campaign ends up
  # measuring a configuration the node no longer has after the next restart.
  "${LAB_REMOTE_SSH[@]}" "$TARGET" "
    set -e
    status=\$(cat /sys/devices/system/cpu/intel_pstate/status 2>/dev/null || echo absent)
    if [ \"\$status\" = absent ]; then
      echo '  runtime: this kernel has no intel_pstate driver - nothing to switch'
    elif [ \"\$status\" = '$PSTATE' ]; then
      echo \"  runtime: already \$status\"
    else
      echo \"  runtime: \$status -> $PSTATE\"
      # Switching the driver resets every CPU's governor to that driver's
      # default, which is why stage 4 sets the governor after this and not before.
      echo '$PSTATE' | $LAB_REMOTE_SUDO tee /sys/devices/system/cpu/intel_pstate/status >/dev/null
    fi

    # A pre-existing intel_pstate= in /etc/default/grub is the interesting case: a
    # drop-in appends to GRUB_CMDLINE_LINUX_DEFAULT, so both would land on one
    # command line and which wins depends on parse order. Refuse to add a second,
    # contradictory one - name the line to edit instead.
    existing=\$(grep -hoE 'intel_pstate=[a-z]+' /etc/default/grub 2>/dev/null | tail -1 || true)
    if [ -n \"\$existing\" ] && [ \"\$existing\" != 'intel_pstate=$PSTATE' ]; then
      echo \"FATAL: /etc/default/grub on this node already sets \$existing.\" >&2
      echo '       Two intel_pstate= arguments on one kernel command line are' >&2
      echo '       ambiguous, so this script will not add a second. Edit that' >&2
      echo \"       line to intel_pstate=$PSTATE (or delete it and re-run), then:\" >&2
      echo '         sudo update-grub' >&2
      exit 3
    fi
    if [ -n \"\$existing\" ]; then
      echo \"  boot:    /etc/default/grub already carries \$existing\"
    else
      $LAB_REMOTE_SUDO sh -c 'cat > /etc/default/grub.d/97-mxl-pstate.cfg <<\"EOF\"
GRUB_CMDLINE_LINUX_DEFAULT=\"\$GRUB_CMDLINE_LINUX_DEFAULT intel_pstate=$PSTATE\"
EOF
update-grub >/dev/null'
      echo '  boot:    wrote /etc/default/grub.d/97-mxl-pstate.cfg (intel_pstate=$PSTATE)'
    fi
  "
fi

echo
echo "== 4/5 governor, EPB, EPP and ELC through PerfSpect =="
# Three invocations rather than one, because they fail for three unrelated
# reasons and a single command line would let the least portable setting take the
# other two down with it.
echo "-- governor=$GOVERNOR, EPB=$EPB (required)"
"${LAB_REMOTE_SSH[@]}" "$TARGET" \
  "cd ~/$LAB_PERFSPECT_DIR && $LAB_REMOTE_SUDO ./perfspect config --gov '$GOVERNOR' --epb '$EPB' --no-summary --noupdate"

echo "-- EPP=$EPP"
if "${LAB_REMOTE_SSH[@]}" "$TARGET" \
     "cd ~/$LAB_PERFSPECT_DIR && $LAB_REMOTE_SUDO ./perfspect config --epp '$EPP' --no-summary --noupdate"; then
  echo "   applied"
else
  # EPP is an HWP control: it exists only while intel_pstate runs in active mode.
  echo "   not applied. EPP needs intel_pstate in active mode (HWP); this node reports"
  echo "   LAB_POWER_PSTATE_DRIVER=$PSTATE. Nothing else was affected."
fi

if [[ "$ELC" == "skip" ]]; then
  echo "-- ELC: skipped (LAB_POWER_ELC=skip)"
else
  echo "-- ELC=$ELC (Sierra Forest and newer only)"
  if "${LAB_REMOTE_SSH[@]}" "$TARGET" \
       "cd ~/$LAB_PERFSPECT_DIR && $LAB_REMOTE_SUDO ./perfspect config --elc '$ELC' --no-summary --noupdate"; then
    echo "   applied"
  else
    echo "   not applied: this CPU does not expose Efficiency Latency Control."
    echo "   Expected on anything older than Sierra Forest - nothing else was"
    echo "   affected. Set LAB_POWER_ELC=skip to stop trying."
  fi
fi

echo
echo "== 5/5 making it survive a reboot =="
# The kernel argument from stage 3 persists by itself; the MSRs and sysfs writes
# do not, so they are re-applied at boot by a unit. It runs the binary from a
# system path, not from a login's home, and it re-applies in the same order as
# above: driver first, because switching it resets the governor.
if [[ "$PSTATE" == "skip" ]]; then
  BOOT_PSTATE=": # LAB_POWER_PSTATE_DRIVER=skip - the driver is left as the kernel found it"
else
  BOOT_PSTATE="[ -w /sys/devices/system/cpu/intel_pstate/status ] && echo $PSTATE > /sys/devices/system/cpu/intel_pstate/status || true"
fi
if [[ "$ELC" == "skip" ]]; then
  BOOT_ELC=": # LAB_POWER_ELC=skip"
else
  BOOT_ELC="$LAB_PERFSPECT_SYSTEM_BIN config --elc $ELC --no-summary --noupdate || true"
fi
"${LAB_REMOTE_SSH[@]}" "$TARGET" "
  set -e
  $LAB_REMOTE_SUDO install -d -m 0755 '$LAB_PERFSPECT_SYSTEM_DIR'
  $LAB_REMOTE_SUDO install -m 0755 ~/$LAB_PERFSPECT_DIR/perfspect '$LAB_PERFSPECT_SYSTEM_BIN'
  $LAB_REMOTE_SUDO sh -c 'cat > /usr/local/sbin/mxl-power-profile <<\"EOF\"
#!/bin/sh
# MXL lab power profile, re-applied at boot by mxl-power-profile.service.
# Generated by scripts/configure-power.sh from config/lab.env - change it there
# and re-run that script, or this file and the lab config will disagree.
set -eu
$BOOT_PSTATE
$LAB_PERFSPECT_SYSTEM_BIN config --gov $GOVERNOR --epb $EPB --no-summary --noupdate
$LAB_PERFSPECT_SYSTEM_BIN config --epp $EPP --no-summary --noupdate || true
$BOOT_ELC
EOF
chmod 0755 /usr/local/sbin/mxl-power-profile'
  $LAB_REMOTE_SUDO sh -c 'cat > /etc/systemd/system/mxl-power-profile.service <<\"EOF\"
[Unit]
Description=MXL lab power profile (P-state driver, governor, EPB, EPP, ELC)
After=multi-user.target

[Service]
Type=oneshot
RemainAfterExit=yes
ExecStart=/usr/local/sbin/mxl-power-profile

[Install]
WantedBy=multi-user.target
EOF
systemctl daemon-reload
systemctl enable --now mxl-power-profile.service'
  echo \"  unit: \$(systemctl is-enabled mxl-power-profile.service), \$(systemctl is-active mxl-power-profile.service)\"
"

echo
if ! verify; then
  echo
  echo "FATAL: the profile did not read back as configured on $NODE." >&2
  echo "       The unit's own output is in 'journalctl -u mxl-power-profile' on" >&2
  echo "       that node. To put the node back as it was:" >&2
  echo "         cd ~/$LAB_PERFSPECT_DIR && sudo ./perfspect config restore \\" >&2
  echo "           ~/$LAB_PERFSPECT_RECORD_DIR/*_config.txt" >&2
  exit 1
fi
if [[ "$VERIFY_EPP_WARNING" -eq 1 ]]; then
  echo
  echo "WARN: EPP is unsupported or unreadable on $NODE (details above)."
  echo "      Required settings (P-state, governor, EPB, and ELC when supported) were verified."
fi

cat <<EOF

Power profile applied on $NODE.

  re-check any time:  scripts/configure-power.sh --verify $NODE
  put it back:        on $NODE, cd ~/$LAB_PERFSPECT_DIR &&
                      sudo ./perfspect config restore ~/$LAB_PERFSPECT_RECORD_DIR/*_config.txt
EOF
if [[ "$PSTATE" != "skip" ]]; then
  cat <<EOF

The runtime driver switch above is already in force, so no reboot is needed to
measure. The kernel argument intel_pstate=$PSTATE only matters for the reboot
after this one - reboot once before the first measured campaign to prove the node
comes back in this state, not between runs of one campaign.
EOF
fi
