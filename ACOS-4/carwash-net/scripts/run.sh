#!/bin/bash
# Запускает на этом компьютере сервер, все посты и источник машин и ждёт сервер.
# Ключи передаются серверу: scripts/run.sh -c data/rush-hour.cfg -d 50
# Адрес и порт - переменные HOST и PORT (по умолчанию 127.0.0.1 и 5050).
# Вывод сервера - в терминал, вывод каждого клиента - в logs/<клиент>.log.
# Наблюдателя запускайте в другом терминале: bin/cw-view 127.0.0.1 5050
# Ctrl+C останавливает сервер, а он - всех клиентов.
cd "$(dirname "$0")/.." || exit 1
BIN=${BIN:-bin}
HOST=${HOST:-127.0.0.1}
PORT=${PORT:-5050}
LOGS=${LOGS:-logs}
CFG=data/normal.cfg
args=("$@")
for ((i = 0; i < ${#args[@]}; i++)); do
    [ "${args[i]}" = "-c" ] && CFG=${args[i + 1]}
done
mkdir -p "$LOGS"

"$BIN/cw-server" "$@" "$HOST" "$PORT" &
server=$!
while read -r name posts; do
    for ((p = 1; p <= posts; p++)); do
        "$BIN/cw-post" "$HOST" "$PORT" "$name" "$p" > "$LOGS/post-$name.$p.log" 2>&1 &
    done
done < <(sed -n 's/#.*//; s/^ *stage *= *\([^ ]*\) *\([0-9]*\).*/\1 \2/p' "$CFG")
"$BIN/cw-cars" "$HOST" "$PORT" > "$LOGS/cars.log" 2>&1 &

trap 'kill -INT $server 2>/dev/null' INT TERM
wait $server
code=$?
while kill -0 $server 2>/dev/null; do
    wait $server
    code=$?
done
wait
exit $code
