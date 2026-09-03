#!/usr/bin/env bash
# CBC FFmpeg-MXL container.
#
# Builds the workload image on a worker from CBC's public build guidance repo and
# imports it into the containerd namespace Kubernetes uses (k8s.io). The image
# carries an FFmpeg with the MXL muxer/demuxer, which is how the decoder Pod
# hands frames to the encoder Pods through shared memory (/dev/shm/mxl) with no
# copy and no network hop.
#
# The image is built ON the worker and never pushed: config/lab.env sets
# LAB_IMAGE_PULL_POLICY=Never so a run can never silently pick up a different
# build from a registry.
#
# Run FROM THE CONTROLLER:
#   scripts/build-ffmpeg-mxl-image.sh [NODE]      # default: LAB_DEFAULT_NODE
#
# Overridables: CBC_REPO, CBC_REF, LAB_IMAGE (from config/lab.env).
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
# shellcheck source=lib/no-proxy.sh
source "$ROOT/scripts/lib/no-proxy.sh"

NODE="${1:-$LAB_DEFAULT_NODE}"
KEY="${NODE^^}_HOST"; KEY="${KEY//-/_}"
HOST="${!KEY:-}"
[[ -n "$HOST" ]] || { echo "FATAL: no address for $NODE ($KEY) in config/nodes.env" >&2; exit 2; }
TARGET="${LAB_SSH_USER}@$HOST"

CBC_REPO="${CBC_REPO:-https://github.com/cbcrc/guidance-for-building-ffmpeg-with-mxl.git}"
# Reference BKC build. Bump deliberately: a different FFmpeg or MXL revision
# changes encoder cost and therefore every density number.
CBC_REF="${CBC_REF:-9b5098a}"
IMAGE="${LAB_IMAGE:-localhost/mxl-ffmpeg-full:v1}"
NERDCTL_VERSION="${NERDCTL_VERSION:-2.3.2}"

# The Dockerfile fetches packages and sources, and a build container inherits none
# of the node's proxy configuration - not /etc/environment, not containerd's own
# drop-in. BuildKit forwards these particular build args into every RUN step, and
# that is the only way in. Without them a proxied build stops at its first apt-get
# or git and stays there until the connection times out, 20 minutes in.
LAB_HTTP_PROXY="${LAB_HTTP_PROXY:-${http_proxy:-${HTTP_PROXY:-}}}"
LAB_HTTPS_PROXY="${LAB_HTTPS_PROXY:-${https_proxy:-${HTTPS_PROXY:-$LAB_HTTP_PROXY}}}"
[[ -n "$LAB_HTTP_PROXY" ]] || LAB_HTTP_PROXY="$LAB_HTTPS_PROXY"
BUILD_NO_PROXY="$(lab_cluster_no_proxy)"
BUILD_PROXY=""
if [[ -n "$LAB_HTTP_PROXY" ]]; then
  # Both spellings of each: which one a build step reads depends on the tool.
  BUILD_PROXY+=" --build-arg http_proxy='$LAB_HTTP_PROXY' --build-arg HTTP_PROXY='$LAB_HTTP_PROXY'"
  BUILD_PROXY+=" --build-arg https_proxy='$LAB_HTTPS_PROXY' --build-arg HTTPS_PROXY='$LAB_HTTPS_PROXY'"
  BUILD_PROXY+=" --build-arg no_proxy='$BUILD_NO_PROXY' --build-arg NO_PROXY='$BUILD_NO_PROXY'"
fi

echo "== building $IMAGE on $NODE ($HOST) from $CBC_REPO @ $CBC_REF =="
[[ -z "$BUILD_PROXY" ]] || echo "   via the proxy $LAB_HTTPS_PROXY, passed into the build as build args"
echo "This takes 15-30 minutes on a cold cache.${LAB_REMOTE_SUDO:+ Enter the worker sudo password when prompted.}"
"${LAB_REMOTE_SSH[@]}" "$TARGET" "
set -Eeuo pipefail

echo '== 1/4 build tools =='
command -v git >/dev/null || $LAB_REMOTE_SUDO apt-get install -y -qq git
if ! command -v nerdctl >/dev/null || ! command -v buildctl >/dev/null || ! command -v buildkitd >/dev/null \
    || [[ ! -f /usr/local/lib/systemd/system/buildkit.service ]]; then
  echo 'installing nerdctl and BuildKit $NERDCTL_VERSION'
  curl -fsSL -o /tmp/nerdctl.tgz \
    https://github.com/containerd/nerdctl/releases/download/v$NERDCTL_VERSION/nerdctl-full-$NERDCTL_VERSION-linux-amd64.tar.gz
  # Extract only the build frontend and backend. The full archive also contains
  # containerd and runc; Kubernetes owns those packages, so do not overwrite them.
  $LAB_REMOTE_SUDO tar -C /usr/local -xzf /tmp/nerdctl.tgz \
    bin/nerdctl bin/buildctl bin/buildkitd lib/systemd/system/buildkit.service
  rm -f /tmp/nerdctl.tgz
fi

# BuildKit pulls base images itself, so build arguments are not enough on a
# proxied network. Its daemon needs the proxy in its systemd environment too.
$LAB_REMOTE_SUDO install -d -m 0755 /etc/systemd/system/buildkit.service.d
if [[ -n '$LAB_HTTP_PROXY' ]]; then
  printf '%s\n' \
    '[Service]' \
    'Environment="HTTP_PROXY=$LAB_HTTP_PROXY"' \
    'Environment="HTTPS_PROXY=$LAB_HTTPS_PROXY"' \
    'Environment="NO_PROXY=$BUILD_NO_PROXY"' \
    | $LAB_REMOTE_SUDO tee /etc/systemd/system/buildkit.service.d/mxl-lab-proxy.conf >/dev/null
else
  $LAB_REMOTE_SUDO rm -f /etc/systemd/system/buildkit.service.d/mxl-lab-proxy.conf
fi
$LAB_REMOTE_SUDO systemctl daemon-reload
$LAB_REMOTE_SUDO systemctl enable --now buildkit.service >/dev/null
$LAB_REMOTE_SUDO systemctl is-active --quiet buildkit.service
nerdctl --version
buildctl --version

echo '== 2/4 source =='
mkdir -p ~/build-ffmpeg-mxl
cd ~/build-ffmpeg-mxl
if [[ ! -d guidance-for-building-ffmpeg-with-mxl/.git ]]; then
  git clone '$CBC_REPO' guidance-for-building-ffmpeg-with-mxl
fi
cd guidance-for-building-ffmpeg-with-mxl
git fetch --all --tags --quiet
git checkout --quiet '$CBC_REF'
echo \"source at \$(git rev-parse --short HEAD)\"

# The pinned CBC production profile enables the MP4/MOV muxers but not their
# demuxer. This lab reads an MP4 source, so without this flag every decoder exits
# immediately with "Invalid data found when processing input". Keep the pinned
# source revision and apply the one required feature explicitly and idempotently.
FFMPEG_STREAMING_OPTIONS=scripts/deps/ffmpeg-configure-streaming-options.txt
grep -qxF -- '--enable-demuxer=mov' "\$FFMPEG_STREAMING_OPTIONS" \
  || printf '%s\n' '--enable-demuxer=mov' >>"\$FFMPEG_STREAMING_OPTIONS"

echo '== 3/4 image build (namespace k8s.io, so the kubelet can see it) =='
$LAB_REMOTE_SUDO nerdctl --namespace k8s.io build$BUILD_PROXY -f docker/prod/Dockerfile -t '$IMAGE' .

echo '== 4/4 verifying the image =='
$LAB_REMOTE_SUDO nerdctl --namespace k8s.io images | grep -F 'mxl-ffmpeg-full'
# The Dockerfile's ENTRYPOINT is CBC's RTSP helper; the lab Pods override it with
# an explicit command. Verify both halves of this lab's data path: MP4 input and
# MXL output.
$LAB_REMOTE_SUDO nerdctl --namespace k8s.io run --rm --net host --entrypoint '${LAB_FFMPEG:-/opt/bin/ffmpeg}' '$IMAGE' \
  -hide_banner -demuxers | grep -Eq '[[:space:]]mov,mp4,' \
  || { echo 'FATAL: built image has no MP4/MOV demuxer' >&2; exit 1; }
$LAB_REMOTE_SUDO nerdctl --namespace k8s.io run --rm --net host --entrypoint '${LAB_FFMPEG:-/opt/bin/ffmpeg}' '$IMAGE' \
  -hide_banner -muxers | grep -E '[[:space:]]mxl[[:space:]]' \
  || { echo 'FATAL: built image has no MXL muxer' >&2; exit 1; }
"

echo
echo "Image $IMAGE is ready on $NODE."
echo "Next: stage the test clips with scripts/stage-media.sh, then scripts/preflight.sh"
