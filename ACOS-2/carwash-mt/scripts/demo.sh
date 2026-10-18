#!/usr/bin/env bash
# Набор запусков для отчёта: вывод каждого - в docs/demo/<имя>.txt вместе с ANSI-кодами (для
# рендера скриншотов). В начале файла - команда, в конце - код выхода. Запуск из любой папки:
#   scripts/demo.sh        (сам собирает build/ и build-tsan/)
# Граф ожидания при тупике дополнительно сохраняется в docs/demo/stall.dot и stall.png (нужен dot).
set -u
ROOT=$(cd "$(dirname "$0")/.." && pwd)
OUT=$ROOT/docs/demo
mkdir -p "$OUT"
BIN=$ROOT/bin/carwash-mt
TSAN_BIN=$ROOT/bin/carwash-mt-tsan

cmake -S "$ROOT" -B "$ROOT/build" >/dev/null && cmake --build "$ROOT/build" --target carwash-mt >/dev/null || exit 1
cmake -S "$ROOT" -B "$ROOT/build-tsan" -DCW_SANITIZE=thread -DCMAKE_BUILD_TYPE=Debug \
    -DCW_NESTED=ON >/dev/null &&
    cmake --build "$ROOT/build-tsan" --target carwash-mt >/dev/null || exit 1

# Рабочая папка запусков: в выводе остаются короткие пути bin/..., data/..., carwash.log
WORK=$(mktemp -d /tmp/carwash-demo.XXXXXX)
trap 'rm -rf "$WORK"' EXIT
ln -s "$ROOT/data" "$WORK/data"
mkdir "$WORK/bin"
ln -s "$BIN" "$WORK/bin/carwash-mt"
ln -s "$TSAN_BIN" "$WORK/bin/carwash-mt-tsan"
cd "$WORK" || exit 1

# run ИМЯ ПРОГРАММА ключи...: выполняет с пределом 30 с, пишет "$ команда", вывод и код выхода
run() {
    local name=$1
    shift
    {
        echo "\$ $*"
        perl -e 'alarm 30; exec @ARGV' "$@" 2>&1
        echo "[exit code $?]"
    } >"$OUT/$name.txt"
    echo "demo: $name"
}

# Оставляет в файле живого режима только начало (скрыть курсор) и последние N кадров с итогами:
# весь день - сотни кадров по несколько килобайт. Кадр начинается с ESC[H.
# lastframes ФАЙЛ N КОМАНДА...
lastframes() {
    python3 - "$1" "$2" <<'PY'
import sys
path, n = sys.argv[1], int(sys.argv[2])
raw = open(path, encoding="utf-8", errors="replace").read()
head, *frames = raw.split("\x1b[H")
last = frames[-1]
tail_at = last.find("\x1b[J")
# в последнем «кадре» после ESC[J идут итоги: оставляем их целиком
keep = frames[-n:] if n <= len(frames) else frames
out = head + "".join("\x1b[H" + f for f in keep)
open(path, "w", encoding="utf-8").write(out)
PY
}

# midframe ФАЙЛ: оставляет один кадр из середины дня (самый заполненный по числу машин «inside»)
midframe() {
    python3 - "$1" <<'PY'
import re, sys
path = sys.argv[1]
raw = open(path, encoding="utf-8", errors="replace").read()
head, *frames = raw.split("\x1b[H")
best = max(frames[:-1], key=lambda f: int((re.search(r"inside (\d+)", f) or [0, 0])[1]))
open(path, "w", encoding="utf-8").write(head + "\x1b[H" + best + "\x1b[J\x1b[0m\x1b[?25h\n")
PY
}

# 1. Обычный день: лента событий и итоги
run normal-log bin/carwash-mt -m log -s 42 -d 0

# 2. Обычный день, живая схема: последние кадры, затем итоги
{
    echo "\$ bin/carwash-mt -s 42 -d 0   (live view, last frames)"
    perl -e 'alarm 30; exec @ARGV' bin/carwash-mt -m live -s 42 -d 0 >"$WORK/live.raw" 2>&1
    code=$?
    lastframes "$WORK/live.raw" 2
    cat "$WORK/live.raw"
    echo "[exit code $code]"
} >"$OUT/normal-live.txt"
echo "demo: normal-live"

# 2б. Живая схема в разгар часа пик: самый загруженный кадр
perl -e 'alarm 30; exec @ARGV' bin/carwash-mt -m live -c data/rush-hour.cfg -s 7 -d 0 \
    >"$WORK/rush.raw" 2>&1
midframe "$WORK/rush.raw"
{
    echo "\$ bin/carwash-mt -c data/rush-hour.cfg -s 7 -d 0   (live view, the busiest frame)"
    cat "$WORK/rush.raw"
} >"$OUT/rush-hour-live.txt"
echo "demo: rush-hour-live"

# 3. Час пик
run rush-hour bin/carwash-mt -m log -c data/rush-hour.cfg -s 7 -d 0

# 4. Пост с ёмкостью 2
run capacity bin/carwash-mt -m log -c data/capacity.cfg -s 5 -d 0

# 5. Тупик: граф ожидания с названной причиной (-D: одинаковый результат при каждом запуске)
run stall bin/carwash-mt -m log -c data/dead-stage.cfg -s 1 -d 0 -D -g stall.dot
if [ -s stall.dot ]; then
    cp stall.dot "$OUT/stall.dot"
    if command -v dot >/dev/null; then
        dot -Tpng stall.dot -o "$OUT/stall.png" && echo "demo: stall.png"
    else
        echo "demo: dot not found, stall.png skipped (brew install graphviz)"
    fi
fi

# 6. SIGUSR1: снимок (граф ожидания и таблица потоков) в stderr, работа идёт дальше; потом SIGINT
{
    echo "\$ bin/carwash-mt -m log -u -s 1 -d 20 &   # kill -USR1, then kill -INT"
    bin/carwash-mt -m log -u -s 1 -d 20 >"$WORK/usr1.out" 2>&1 &
    pid=$!
    sleep 1.5
    kill -USR1 $pid
    sleep 1
    kill -INT $pid
    wait $pid
    code=$?
    cat "$WORK/usr1.out"
    echo "[exit code $code]"
} >"$OUT/sigusr1.txt"
echo "demo: sigusr1"

# 7. SIGUSR2: въезд закрывается досрочно, принятые машины домываются, код 0
{
    echo "\$ bin/carwash-mt -m log -u -s 1 -d 20 &   # kill -USR2"
    bin/carwash-mt -m log -u -s 1 -d 20 >"$WORK/usr2.out" 2>&1 &
    pid=$!
    sleep 1.5
    kill -USR2 $pid
    wait $pid
    code=$?
    cat "$WORK/usr2.out"
    echo "[exit code $code]"
} >"$OUT/sigusr2.txt"
echo "demo: sigusr2"

# 8-9. Саботаж: переполнение очереди (аудитор и SOFT_ASSERT) и взаимная блокировка (LOCKDEP)
run sabotage-overflow bin/carwash-mt -m log -x overflow -c data/rush-hour.cfg -s 3 -d 0
run sabotage-deadlock env TSAN_OPTIONS=report_thread_leaks=0 bin/carwash-mt -m log -x deadlock \
    -c data/rush-hour.cfg -s 1 -d 0

# 10. Саботаж: гонка данных, её видит только ThreadSanitizer (сборка -tsan, первый отчёт)
run sabotage-race env TSAN_OPTIONS=halt_on_error=1:abort_on_error=0 bin/carwash-mt-tsan -m log \
    -x race -c data/rush-hour.cfg -s 1 -d 0

echo "demo: files are in $OUT"
