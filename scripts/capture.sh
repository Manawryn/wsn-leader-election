#!/bin/bash
# capture.sh [outdir] - log every connected TelosB to <outdir>/<port>.log
# Run inside the VM from the project folder; stop with Ctrl+C.
#   ./scripts/capture.sh results/exp1
# To send a command to one mote while capturing:  echo "fail 0" > /dev/ttyUSB2
cd "$(dirname "$0")/.." || exit 1
dir=${1:-results/$(date +%Y%m%d-%H%M)}
mkdir -p "$dir"
SD=../../tools/serial-io/serialdump
[ -x "$SD" ] || make -C ../../tools/serial-io serialdump >/dev/null || exit 1
ports=$(ls /dev/ttyUSB* 2>/dev/null)
[ -n "$ports" ] || { echo "no /dev/ttyUSB* found (motes connected to the VM?)"; exit 1; }
for p in $ports; do
  "$SD" -b115200 "$p" > "$dir/$(basename "$p").log" 2>/dev/null &
  echo "logging $p -> $dir/$(basename "$p").log"
done
trap 'kill $(jobs -p) 2>/dev/null; echo; echo "analyse with: python3 scripts/analyze.py $dir/*.log"' INT
wait
