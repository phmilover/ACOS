#!/bin/bash
# Программа 4: запускает координатора, все посты и источник машин как независимые процессы
# и ждёт их. Ключи передаются координатору: scripts/run4.sh -c data/rush-hour.cfg -d 50
# Ctrl+C останавливает всю линию.
cd "$(dirname "$0")/.." || exit 1
BIN=${BIN:-bin}
KEY=${KEY:-cw4-$$}
CFG=data/normal.cfg
args=("$@")
for ((i = 0; i < ${#args[@]}; i++)); do
    [ "${args[i]}" = "-c" ] && CFG=${args[i + 1]}
done

"$BIN/cw4-coord" -k "$KEY" "$@" &
coord=$!
while read -r name posts; do
    for ((p = 1; p <= posts; p++)); do
        "$BIN/cw4-post" -k "$KEY" "$name" "$p" &
    done
done < <(sed -n 's/#.*//; s/^ *stage *= *\([^ ]*\) *\([0-9]*\).*/\1 \2/p' "$CFG")
"$BIN/cw4-cars" -k "$KEY" &

trap 'kill -INT $coord 2>/dev/null' INT TERM
wait $coord
code=$?
while kill -0 $coord 2>/dev/null; do
    wait $coord
    code=$?
done
wait
exit $code
