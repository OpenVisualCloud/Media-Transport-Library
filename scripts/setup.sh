#!/usr/bin/env bash
# Install the controller-side runner (the `mxl-perf` command) into .venv/.
#
# Everything else in this repo calls .venv/bin/mxl-perf, so this is the only
# Python setup step. If python3-venv is unavailable (common on a locked-down
# controller), it falls back to a thin wrapper around the system interpreter,
# which needs python3-yaml, python3-requests and python3-openpyxl installed.
set -Eeuo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
command -v python3 >/dev/null || { echo "FATAL: python3 is not installed" >&2; exit 2; }

rm -rf "$ROOT/.venv"

if python3 -m venv "$ROOT/.venv" 2>/dev/null && [[ -x "$ROOT/.venv/bin/pip" ]]; then
  "$ROOT/.venv/bin/python" -m pip install --quiet --upgrade pip
  # [dev] adds pytest, so 'make test' works straight after this.
  "$ROOT/.venv/bin/pip" install --quiet -e "$ROOT[dev]"
  echo "Installed into a virtualenv."
else
  echo "python3-venv unavailable; wrapping the system interpreter instead."
  if ! python3 - <<'PY'
import openpyxl, requests, yaml
print("system dependencies present: openpyxl", openpyxl.__version__,
      "requests", requests.__version__, "pyyaml", yaml.__version__)
PY
  then
    cat >&2 <<'EOF'
FATAL: no virtualenv and the system interpreter is missing dependencies.
The simple fix is the venv:
    sudo apt-get install -y python3-venv
    scripts/setup.sh
Or install the dependencies system-wide instead:
    sudo apt-get install -y python3-yaml python3-requests python3-openpyxl
EOF
    exit 2
  fi
  rm -rf "$ROOT/.venv"
  mkdir -p "$ROOT/.venv/bin"
  for name in python mxl-perf; do
    target="$ROOT/.venv/bin/$name"
    printf '#!/usr/bin/env bash\nexport PYTHONPATH="%s/python${PYTHONPATH:+:$PYTHONPATH}"\n' "$ROOT" >"$target"
    if [[ "$name" == "python" ]]; then
      printf 'exec %s "$@"\n' "$(command -v python3)" >>"$target"
    else
      printf 'exec %s -m mxlperf.cli "$@"\n' "$(command -v python3)" >>"$target"
    fi
    chmod +x "$target"
  done
fi

"$ROOT/.venv/bin/mxl-perf" --help >/dev/null
echo "Ready. Try: scripts/preflight.sh"
