#!/bin/bash
# Сценарные тесты сервера и клиентов: tests/run_tests.sh [BIN]
#
# Все сценарии из data/ (код выхода, ноль нарушений инвариантов, все принятые машины вымыты,
# все клиенты завершились вместе с сервером), сигналы серверу, отключение и повторное
# подключение постов, источника машин и наблюдателей посреди дня, отказы в регистрации.
# Всё запускается на 127.0.0.1, каждый тест - на своём порту.
cd "$(dirname "$0")/.." || exit 1
BIN=${1:-bin}
export BIN
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
DELAY=${DELAY:-3}
PORT=$((5100 + RANDOM % 400))
pass=0
fail=0

ok() {
    pass=$((pass + 1))
    printf '  ok   %s\n' "$1"
}
bad() {
    fail=$((fail + 1))
    printf '  FAIL %s\n' "$1"
    [ -n "$2" ] && sed 's/^/       /' "$2" | tail -6
}

next_port() {
    PORT=$((PORT + 1))
}

# Ждёт процесс $1 не дольше $2 секунд; завис - SIGINT, затем SIGKILL. Код - в $code.
wait_for() {
    local pid=$1 limit=$(($2 * 10)) t=0
    while kill -0 "$pid" 2>/dev/null && [ $t -lt $limit ]; do
        sleep 0.1
        t=$((t + 1))
    done
    if kill -0 "$pid" 2>/dev/null; then
        kill -INT "$pid" 2>/dev/null
        sleep 2
        kill -KILL "$pid" 2>/dev/null
        code=hang
        wait "$pid" 2>/dev/null
        return
    fi
    wait "$pid"
    code=$?
}

# Остались ли процессы этого порта (клиенты должны уйти вместе с сервером).
leftovers() {
    pgrep -f "cw-[a-z]+ .*127.0.0.1 $1( |$)"
}

server_pid() {
    pgrep -f "cw-server .*127.0.0.1 $1$"
}

expect_code() {
    case $1 in
    bad-route) echo 1 ;;
    impossible) echo 2 ;;
    *) echo 0 ;;
    esac
}

echo "== scenarios from data/"
for cfg in data/*.cfg; do
    name=$(basename "$cfg" .cfg)
    next_port
    rm -rf "$TMP/logs"
    LOGS="$TMP/logs" PORT=$PORT scripts/run.sh -q -d "$DELAY" -s 11 -c "$cfg" -l "$TMP/$name.log" \
        > "$TMP/out" 2>&1 &
    wait_for $! 60
    want=$(expect_code "$name")
    sleep 0.3
    stopped=$(grep -l "server stopped the line" "$TMP"/logs/*.log 2>/dev/null | wc -l | tr -d " ")
    clients=$(ls "$TMP"/logs/*.log 2>/dev/null | wc -l | tr -d " ")
    if [ "$code" != "$want" ]; then
        bad "$name: exit code $code, expected $want" "$TMP/out"
    elif [ "$want" = 1 ]; then
        ok "$name: bad configuration rejected (exit 1)"
    elif ! grep -q "Invariant violations: 0" "$TMP/out"; then
        bad "$name: invariant violated" "$TMP/out"
    elif [ "$want" = 0 ] && ! grep -q "left unfinished 0" "$TMP/out"; then
        bad "$name: some accepted cars are not washed" "$TMP/out"
    elif [ "$want" = 0 ] && [ "$stopped" != "$clients" ]; then
        bad "$name: only $stopped of $clients clients were stopped by the server" "$TMP/out"
    elif leftovers $PORT > /dev/null; then
        bad "$name: client processes are left" "$TMP/out"
    else
        ok "$name: exit $code, $(grep -o 'washed [0-9]*' "$TMP/out" | head -1), $clients clients stopped with the server"
    fi
done

echo "== signals to the server"
next_port
rm -rf "$TMP/logs"
LOGS="$TMP/logs" PORT=$PORT scripts/run.sh -q -u -d 5 -l "$TMP/i.log" > "$TMP/out" 2>&1 &
run=$!
sleep 1.5
srv=$(server_pid $PORT)
kill -USR1 "$srv"
sleep 0.3
# фоновый скрипт наследует игнорируемый SIGINT, поэтому шлём серверу, как Ctrl+C в терминале
kill -INT "$srv"
wait_for $run 15
sleep 0.3
if [ "$code" = 3 ] && grep -q "status at" "$TMP/out" && grep -q "posts online" "$TMP/out" &&
    grep -q "interrupted" "$TMP/i.log" && ! leftovers $PORT > /dev/null &&
    grep -q "server stopped the line" "$TMP/logs/cars.log"; then
    ok "SIGUSR1 prints the state, SIGINT stops the server and every client (exit 3)"
else
    bad "SIGUSR1/SIGINT: exit $code" "$TMP/out"
fi

next_port
LOGS="$TMP/logs" PORT=$PORT scripts/run.sh -q -u -d 5 -l "$TMP/u.log" > "$TMP/out" 2>&1 &
run=$!
sleep 1.5
kill -USR2 "$(server_pid $PORT)"
wait_for $run 30
if [ "$code" = 0 ] && grep -q "closed early by SIGUSR2" "$TMP/u.log" &&
    grep -q "left unfinished 0" "$TMP/out"; then
    ok "SIGUSR2 closes the entrance, accepted cars are finished (exit 0)"
else
    bad "SIGUSR2: exit $code" "$TMP/out"
fi

echo "== clients leave and come back"
next_port
rm -rf "$TMP/logs"
LOGS="$TMP/logs" PORT=$PORT scripts/run.sh -q -d 40 -s 5 -c data/normal.cfg -l "$TMP/r.log" \
    > "$TMP/out" 2>&1 &
run=$!
"$BIN/cw-view" -p 127.0.0.1 $PORT > "$TMP/view1.out" 2>&1 &
view1=$!
"$BIN/cw-view" -p 127.0.0.1 $PORT > "$TMP/view2.out" 2>&1 &
view2=$!
sleep 3
kill -INT "$(pgrep -f "cw-post 127.0.0.1 $PORT tunnel 1$")" "$(pgrep -f "cw-cars 127.0.0.1 $PORT$")"
kill -INT $view1
wait $view1
v1=$?
sleep 2
"$BIN/cw-post" 127.0.0.1 $PORT tunnel 1 > "$TMP/post-again.out" 2>&1 &
"$BIN/cw-cars" 127.0.0.1 $PORT > "$TMP/cars-again.out" 2>&1 &
"$BIN/cw-view" -p 127.0.0.1 $PORT > "$TMP/view3.out" 2>&1 &
view3=$!
wait_for $run 90
wait $view2 $view3
sleep 0.3
if [ "$code" = 0 ] && grep -q "Invariant violations: 0" "$TMP/out" &&
    grep -q "left unfinished 0" "$TMP/out"; then
    ok "the day ends normally (exit 0, no violations, every accepted car washed)"
else
    bad "reconnect day: exit $code" "$TMP/out"
fi
if grep -q "tunnel.1 .*post is offline" "$TMP/r.log" && grep -q "tunnel.1 .*back online" "$TMP/r.log" &&
    grep -q "leaves the line" "$TMP/logs/post-tunnel.1.log" &&
    grep -q "server stopped the line" "$TMP/post-again.out"; then
    ok "a post leaves by Ctrl+C, the line goes on, the restarted post returns to work"
else
    bad "post reconnect" "$TMP/r.log"
fi
if grep -q "car source .*no new cars" "$TMP/r.log" && grep -q "car source is back online" "$TMP/r.log" &&
    grep -q "still to come" "$TMP/cars-again.out" &&
    ! grep -q '^\[08:0.\] entrance' "$TMP/cars-again.out" &&
    [ "$(grep -c "arrives, chooses" "$TMP/r.log")" = 40 ]; then
    ok "the car source leaves and returns with the right clock, the day still has exactly 40 cars"
else
    bad "car source reconnect: $(grep -c "arrives, chooses" "$TMP/r.log") cars" "$TMP/cars-again.out"
fi
if [ "$v1" = 0 ] && grep -q "viewer closed" "$TMP/view1.out" && grep -q "Summary" "$TMP/view2.out" &&
    grep -q "Summary" "$TMP/view3.out" && grep -q "stopped the line" "$TMP/view3.out"; then
    ok "viewers connect and leave at any time; all present get the summary"
else
    bad "viewers" "$TMP/view2.out"
fi

echo "== refusals"
next_port
"$BIN/cw-server" -q -l "$TMP/x.log" 127.0.0.1 $PORT > "$TMP/srv.out" 2>&1 &
srv=$!
sleep 0.3
"$BIN/cw-post" 127.0.0.1 $PORT tunnel 1 > /dev/null 2>&1 &
sleep 0.3
"$BIN/cw-post" 127.0.0.1 $PORT tunnel 1 > "$TMP/dup.out" 2>&1
c1=$?
"$BIN/cw-post" 127.0.0.1 $PORT polish 1 > "$TMP/stage.out" 2>&1
c2=$?
"$BIN/cw-post" 127.0.0.1 $PORT tunnel 9 > "$TMP/num.out" 2>&1
c3=$?
"$BIN/cw-server" -q -l "$TMP/x2.log" 127.0.0.1 $PORT > "$TMP/busy.out" 2>&1
c4=$?
printf 'HELLO robot 1\n' | nc -w 1 127.0.0.1 $PORT > "$TMP/nc.out" 2>&1
kill -INT $srv
wait_for $srv 10
if [ $c1 = 1 ] && grep -q "already online" "$TMP/dup.out" && [ $c2 = 1 ] &&
    grep -q "no stage 'polish'" "$TMP/stage.out" && [ $c3 = 1 ] && grep -q "posts 1..2" "$TMP/num.out"; then
    ok "a duplicate post, an unknown stage and a wrong post number are refused"
else
    bad "post refusals: $c1 $c2 $c3" "$TMP/dup.out"
fi
if [ $c4 = 5 ] && grep -q "Address already in use" "$TMP/busy.out"; then
    ok "a second server on a busy port exits with code 5"
else
    bad "busy port: exit $c4" "$TMP/busy.out"
fi
if grep -q "REJECT expected" "$TMP/nc.out"; then
    ok "a stranger speaking the wrong protocol is refused"
else
    bad "wrong protocol" "$TMP/nc.out"
fi
"$BIN/cw-server" -q 127.0.0.1 > /dev/null 2>&1
c5=$?
"$BIN/cw-post" 127.0.0.1 99999 tunnel 1 > /dev/null 2>&1
c6=$?
"$BIN/cw-server" -q -l "$TMP/ip.log" 300.1.1.1 5000 > "$TMP/ip.out" 2>&1
c7=$?
if [ $c5 = 1 ] && [ $c6 = 1 ] && [ $c7 = 5 ] && grep -q "bad IPv4" "$TMP/ip.out"; then
    ok "missing port, bad port and bad address are reported"
else
    bad "bad arguments: $c5 $c6 $c7" "$TMP/ip.out"
fi

echo "passed $pass, failed $fail"
[ $fail = 0 ]
