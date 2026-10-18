#!/usr/bin/env bash
# Deploy the PyTorch CPU runtime to the NNP-I card.
#
# Required environment:
#   NNPI_SSH_KEY   path to the SSH private key for the card
# Optional environment:
#   NNPI_CARD          default root@192.168.115.2
#   NNPI_WSL_DISTRO    default NNPI-Build
#   NNPI_WSL_ASSETS    default /root/card-py (inside the WSL distro)
#
# Prerequisites (host side):
#   - WSL distro with /root/card-py/{py,wheels,wheels3,resnet18.pth}
#   - working SSH access to the card
# Usage: NNPI_SSH_KEY=/path/to/key ./deploy.sh
set -euo pipefail

KEY="${NNPI_SSH_KEY:?set NNPI_SSH_KEY to the card SSH private key}"
CARD="${NNPI_CARD:-root@192.168.115.2}"
WSL_DISTRO="${NNPI_WSL_DISTRO:-NNPI-Build}"
ASSETS="${NNPI_WSL_ASSETS:-/root/card-py}"
HERE="$(cd "$(dirname "$0")" && pwd)"

SSH=(ssh -F /dev/null -i "$KEY" -o BatchMode=yes -o IdentitiesOnly=yes
     -o IdentityAgent=none -o StrictHostKeyChecking=yes
     -o UserKnownHostsFile="${NNPI_KNOWN_HOSTS:-$HOME/.ssh/known_hosts}" "$CARD")

# 1) Runtime, wheels and weights from WSL. Keep diagnostics off the tar stream.
#    wheels4 / ONNX assets are optional; deployed only when present in WSL.
wsl -d "$WSL_DISTRO" -e bash -lc '
    list="py wheels wheels3 resnet18.pth"
    [ -d '"$ASSETS"'/wheels4 ] && [ -f '"$ASSETS"'/resnet18.onnx ] \
        && list="$list wheels4 resnet18.onnx resnet_input.pt"
    tar -C '"$ASSETS"' -cf - $list' \
  | "${SSH[@]}" 'mkdir -p /tmp/card-py && tar -xf - -C /tmp/card-py'

# 2) Helper scripts from this repository (not present in WSL).
tar -C "$HERE" -cf - install_wheels.py resnet_check.py transformer_check.py ort_check.py \
  | "${SSH[@]}" 'tar -xf - -C /tmp/card-py'

# 3) Generate the launcher and unpack wheels only when site/torch is absent.
"${SSH[@]}" '
      cat > /tmp/card-py/run.sh <<"EOS"
#!/bin/sh
export PYTHONHOME=/tmp/card-py/py
export PYTHONPATH=/tmp/card-py/site
export LD_LIBRARY_PATH=/tmp/card-py/py/lib
export OMP_NUM_THREADS=${OMP_NUM_THREADS:-2}
exec /tmp/card-py/py/bin/python3.8 "$@"
EOS
      chmod +x /tmp/card-py/run.sh
      if [ ! -d /tmp/card-py/site/torch ]; then
          PYTHONHOME=/tmp/card-py/py LD_LIBRARY_PATH=/tmp/card-py/py/lib \
              /tmp/card-py/py/bin/python3.8 /tmp/card-py/install_wheels.py \
              /tmp/card-py/site /tmp/card-py/wheels /tmp/card-py/wheels3
      fi
      if [ -d /tmp/card-py/wheels4 ] && [ ! -d /tmp/card-py/site/onnxruntime ]; then
          PYTHONHOME=/tmp/card-py/py LD_LIBRARY_PATH=/tmp/card-py/py/lib \
              /tmp/card-py/py/bin/python3.8 /tmp/card-py/install_wheels.py \
              /tmp/card-py/site /tmp/card-py/wheels4
      fi
      /tmp/card-py/run.sh - <<"CHECK"
import torch, torchvision, numpy
print("ready", torch.__version__, torchvision.__version__, numpy.__version__)
try:
    import onnxruntime
    print("onnxruntime", onnxruntime.__version__)
except ImportError:
    pass
CHECK
    '
