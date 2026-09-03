#!/usr/bin/env bash

# Select the remote privilege command and SSH mode for a configured login.
# Root needs neither sudo nor a TTY; a normal administrator needs both.
#
# Two SSH modes, because -t is wrong for half the uses:
#   LAB_REMOTE_SSH          interactive: a TTY so sudo can prompt for a password.
#   LAB_REMOTE_SSH_CAPTURE  for "$(...)": never a TTY. A pty turns every remote
#                           newline into CR LF, so a captured value arrives as
#                           $'1\r' and every string comparison against it fails;
#                           it also merges the remote stderr and any password
#                           prompt into the captured stdout. Commands run this
#                           way must therefore need no password.
lab_remote_admin_init() {
  local user="$1"
  LAB_REMOTE_SSH_CAPTURE=(ssh -o BatchMode=yes)
  if [[ "$user" == "root" ]]; then
    LAB_REMOTE_SUDO=""
    LAB_REMOTE_SSH=(ssh -o BatchMode=yes)
  else
    LAB_REMOTE_SUDO="sudo"
    LAB_REMOTE_SSH=(ssh -t)
  fi
}
