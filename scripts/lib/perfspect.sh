#!/usr/bin/env bash

# One definition of where PerfSpect lives on a worker and how it gets there.
# Both the baseline report (scripts/run-perfspect.sh) and the power profile
# (scripts/configure-power.sh) need the same binary, and a second copy in a
# second place is a second version to keep straight.

LAB_PERFSPECT_DIR="perfspect"                       # relative to the login's home
# Where 'perfspect config --record' parks the pre-change configuration. It is the
# only way back from a wrong MSR, so it lives outside the unpack directory that
# an upgrade wipes.
LAB_PERFSPECT_RECORD_DIR="perfspect-config-record"
# Where the boot-time unit reads the binary from: a system path, because a
# systemd unit must not depend on a user's home directory being present or
# readable at boot.
LAB_PERFSPECT_SYSTEM_DIR="/opt/mxl/perfspect"
LAB_PERFSPECT_SYSTEM_BIN="$LAB_PERFSPECT_SYSTEM_DIR/perfspect"

# Download and unpack PerfSpect on the target if it is not already there, and
# print the version either way. Needs no privilege: it installs into the login's
# home, so it runs over a TTY-less connection.
#   lab_perfspect_install <ssh-target>
lab_perfspect_install() {
  local target="$1"
  ssh -o BatchMode=yes "$target" "
    set -e
    if [ ! -x ~/$LAB_PERFSPECT_DIR/perfspect ]; then
      echo 'downloading PerfSpect'
      cd ~ && rm -rf $LAB_PERFSPECT_DIR perfspect.tgz
      curl -fsSL -O https://github.com/intel/PerfSpect/releases/latest/download/perfspect.tgz
      tar -xf perfspect.tgz && rm -f perfspect.tgz
    fi
    ~/$LAB_PERFSPECT_DIR/perfspect --version
  "
}
