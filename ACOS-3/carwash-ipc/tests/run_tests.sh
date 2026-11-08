#!/bin/bash
# Сценарные тесты всех четырёх программ: tests/run_tests.sh [BIN]
#
# Для каждой программы: все сценарии из data/ (код выхода, ноль нарушений инвариантов, все
# принятые машины вымыты), сигналы SIGINT, SIGUSR1, SIGUSR2, отказ второму координатору с тем
# же ключом и то, что после выхода в системе не осталось сегментов, семафоров и каналов.
# Программы 3 и 4 запускаются через scripts/run3.sh и scripts/run4.sh.
cd "$(dirname "$0")/.." || exit 1
BIN=${1:-bin}
export BIN
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
DELAY=${DELAY:-2}
pass=0
fail=0

ok() {
    pass=$((pass + 1))
    printf '  ok   %s\n' "$1"
}
bad() {
    fail=$((fail + 1))
    printf '  FAIL %s\n' "$1"
    [ -n "$2" ] && sed 's/^/       /' "$2" | tail -5
}

# Запускает в фоне программу $1 (1..4) с ключом $2, остальные аргументы - программе; pid - в $!.
start() {
    local n=$1 key=$2
    shift 2
    case $n in
    1 | 2) "$BIN/cw$n" "$@" & ;;
    3 | 4) KEY=$key "scripts/run$n.sh" "$@" & ;;
    esac
}

# Ключ объектов IPC: у программ 1 и 2 он содержит pid координатора.
keyof() {
    local n=$1 key=$2 pid=$3
    case $n in
    1 | 2) echo "cw$n-$pid" ;;
    *) echo "$key" ;;
    esac
}

# Зависший прогон: SIGINT, через 5 с - SIGKILL ему и его детям (участникам программ 3 и 4).
stop_hung() {
    local pid=$1
    kill -INT "$pid" 2>/dev/null
    for _ in 1 2 3 4 5 6 7 8 9 10; do
        kill -0 "$pid" 2>/dev/null || return
        sleep 0.5
    done
    kill -KILL $(pgrep -P "$pid") "$pid" 2>/dev/null
}

expect_code() {
    case $1 in
    bad-route) echo 1 ;;
    impossible) echo 2 ;;
    *) echo 0 ;;
    esac
}

for n in 1 2 3 4; do
    echo "== program $n"
    if [ "$n" = 2 ] && [ "$(uname -s)" != Linux ]; then
        "$BIN/cw2" -q -d 1 -l "$TMP/x.log" > "$TMP/x.out" 2>&1
        if [ $? = 5 ] && grep -q "Function not implemented" "$TMP/x.out"; then
            ok "cw2 reports that unnamed semaphores are not supported here (skip the rest)"
        else
            bad "cw2 on $(uname -s)" "$TMP/x.out"
        fi
        continue
    fi

    for cfg in data/*.cfg; do
        name=$(basename "$cfg" .cfg)
        key="t$n-${name:0:6}-$$"
        log="$TMP/$name.log"
        start $n "$key" -q -d "$DELAY" -s 11 -c "$cfg" -l "$log" > "$TMP/out" 2>&1
        pid=$!
        timeout=0
        while kill -0 "$pid" 2>/dev/null && [ $timeout -lt 600 ]; do
            sleep 0.1
            timeout=$((timeout + 1))
        done
        if kill -0 "$pid" 2>/dev/null; then
            stop_hung "$pid"
            bad "$name: hangs" "$TMP/out"
            wait "$pid"
            continue
        fi
        wait "$pid"
        code=$?
        want=$(expect_code "$name")
        k=$(keyof $n "$key" "$pid")
        if [ "$code" != "$want" ]; then
            bad "$name: exit code $code, expected $want" "$TMP/out"
        elif [ "$want" = 1 ]; then
            ok "$name: bad configuration rejected (exit 1)"
        elif ! grep -q "Invariant violations: 0" "$TMP/out"; then
            bad "$name: invariant violated" "$TMP/out"
        elif [ "$want" = 0 ] && ! grep -q "left unfinished 0" "$TMP/out"; then
            bad "$name: some accepted cars are not washed" "$TMP/out"
        elif ! "$BIN/ipc-left" "$k" > "$TMP/left"; then
            bad "$name: IPC objects left" "$TMP/left"
        else
            ok "$name: exit $code, $(grep -o 'washed [0-9]*' "$TMP/out" | head -1), no IPC left"
        fi
    done

    # SIGUSR1 - состояние, затем SIGINT - остановка с кодом 3
    key="t$n-int-$$"
    start $n "$key" -q -u -d 5 -l "$TMP/i.log" > "$TMP/out" 2>&1
        pid=$!
    sleep 1.5
    coord=$pid
    [ "$n" -ge 3 ] && coord=$(pgrep -f "cw$n-coord -k $key")
    kill -USR1 "$coord"
    sleep 0.3
    # фоновый скрипт run3/run4 наследует игнорируемый SIGINT, поэтому шлём координатору,
    # как и Ctrl+C в терминале (там его получают все процессы группы)
    kill -INT "$coord"
    wait "$pid"
    code=$?
    k=$(keyof $n "$key" "$pid")
    if [ $code = 3 ] && grep -q "status at" "$TMP/out" && grep -q "interrupted" "$TMP/i.log" &&
        "$BIN/ipc-left" "$k" > "$TMP/left"; then
        ok "SIGUSR1 prints the state, SIGINT stops everything (exit 3), no IPC left"
    else
        bad "SIGUSR1/SIGINT: exit $code" "$TMP/out"
    fi

    # SIGUSR2 - въезд закрывается, принятые машины домываются, код 0
    key="t$n-usr2-$$"
    start $n "$key" -q -u -d 5 -l "$TMP/u.log" > "$TMP/out" 2>&1
        pid=$!
    sleep 1.5
    coord=$pid
    [ "$n" -ge 3 ] && coord=$(pgrep -f "cw$n-coord -k $key")
    kill -USR2 "$coord"
    wait "$pid"
    code=$?
    if [ $code = 0 ] && grep -q "closed early by SIGUSR2" "$TMP/u.log" &&
        grep -q "left unfinished 0" "$TMP/out"; then
        ok "SIGUSR2 closes the entrance, accepted cars are finished (exit 0)"
    else
        bad "SIGUSR2: exit $code" "$TMP/out"
    fi

    if [ "$n" -ge 3 ]; then
        # пост, прерванный по Ctrl+C в своём терминале, останавливает всю линию
        key="t$n-post-$$"
        start $n "$key" -q -u -d 5 -l "$TMP/p.log" > "$TMP/out" 2>&1
        pid=$!
        sleep 1.5
        kill -INT "$(pgrep -f "cw$n-post -k $key tunnel 1")"
        wait "$pid"
        code=$?
        if [ $code = 3 ] && "$BIN/ipc-left" "$key" > "$TMP/left"; then
            ok "SIGINT to one post stops the whole line (exit 3), no IPC left"
        else
            bad "SIGINT to a post: exit $code" "$TMP/out"
        fi

        # второй координатор с тем же ключом получает отказ, первый не страдает
        key="t$n-dup-$$"
        "$BIN/cw$n-coord" -k "$key" -q -l "$TMP/d1.log" > "$TMP/out" 2>&1 &
        first=$!
        sleep 0.5
        "$BIN/cw$n-coord" -k "$key" -q -l "$TMP/d2.log" > "$TMP/out2" 2>&1
        code=$?
        kill -INT $first
        wait $first
        code1=$?
        if [ $code = 5 ] && grep -q "in use" "$TMP/out2" && [ $code1 = 3 ] &&
            "$BIN/ipc-left" "$key" > "$TMP/left"; then
            ok "second coordinator with the same key is refused (exit 5)"
        else
            bad "duplicate coordinator: exit $code / $code1" "$TMP/out2"
        fi
    fi
done

echo "passed $pass, failed $fail"
[ $fail = 0 ]
