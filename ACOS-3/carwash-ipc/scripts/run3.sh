#!/bin/bash
# Программа 3: запускает координатора, все посты и источник машин как независимые процессы
# (каждый - отдельной командой в фоне) и ждёт их. Ключи передаются координатору:
#   scripts/run3.sh -c data/rush-hour.cfg -d 50
# Ctrl+C останавливает всю линию.
cd "$(dirname "$0")/.." || exit 1
BIN=${BIN:-bin}
KEY=${KEY:-cw3-$$}
CFG=data/normal.cfg
args=("$@")
for ((i = 0; i < ${#args[@]}; i++)); do
    [ "${args[i]}" = "-c" ] && CFG=${args[i + 1]}
done

"$BIN/cw3-coord" -k "$KEY" "$@" &
coord=$!
# посты: по строкам "stage = имя число ..." конфигурации
while read -r name posts; do
    for ((p = 1; p <= posts; p++)); do
        "$BIN/cw3-post" -k "$KEY" "$name" "$p" &
    done
done < <(sed -n 's/#.*//; s/^ *stage *= *\([^ ]*\) *\([0-9]*\).*/\1 \2/p' "$CFG")
"$BIN/cw3-cars" -k "$KEY" &

trap 'kill -INT $coord 2>/dev/null' INT TERM
# wait прерывается сигналом, поэтому ждём, пока координатор действительно завершится
wait $coord
code=$?
while kill -0 $coord 2>/dev/null; do
    wait $coord
    code=$?
done
wait
exit $code
