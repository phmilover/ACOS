/**
 * @file signals.c
 * @brief Поток сигналов.
 *
 * В обработчике сигнала можно звать только async-signal-safe функции, а мьютексы и условные
 * переменные часов к ним не относятся. Поэтому сигналы заблокированы, и их синхронно забирает
 * обычный поток через sigwait - в нём можно всё. Остановка: флаг quit и pthread_kill(SIGUSR2) - тот
 * же sigwait возвращается и видит флаг.
 */

#include "signals.h"

#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <string.h>

#include "sim.h"
#include "sync.h"
#include "thread.h"

static pthread_t thread;
static int started = 0;  // трогает только главный поток
static atomic_int quit = 0;
static Line *line;  // пишется до ThreadCreate, дальше только читается

static void FillSet(sigset_t *set) {
    sigemptyset(set);
    sigaddset(set, SIGINT);
    sigaddset(set, SIGTERM);
    sigaddset(set, SIGUSR1);
    sigaddset(set, SIGUSR2);
}

void SignalsBlock(void) {
    sigset_t set;
    FillSet(&set);
    pthread_sigmask(SIG_BLOCK, &set, NULL);
    // фоновое задание неинтерактивной оболочки получает SIGINT игнорируемым, а игнорируемый
    // сигнал отбрасывается и до sigwait не доходит. Как обработчик в ИЗ1, возвращаем
    // действие по умолчанию; пока сигнал заблокирован, оно не срабатывает
    const int sigs[] = {SIGINT, SIGTERM, SIGUSR1, SIGUSR2};
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = SIG_DFL;
    sigemptyset(&sa.sa_mask);
    for (size_t i = 0; i < sizeof sigs / sizeof sigs[0]; ++i) {
        sigaction(sigs[i], &sa, NULL);
    }
}

/// Ставит запрос и будит часы: крючок разберёт его в ближайший момент покоя.
static void Request(int bit) {
    atomic_fetch_or(&line->requests, bit);
    SimKick();
}

static void *SignalsMain(void *unused) {
    (void)unused;
    LockdepSetThreadName("signals");
    sigset_t set;
    FillSet(&set);
    for (;;) {
        int sig = 0;
        if (sigwait(&set, &sig) != 0) {
            continue;
        }
        if (atomic_load(&quit)) {
            break;
        }
        switch (sig) {
        case SIGINT:
        case SIGTERM: Request(REQ_INTERRUPT); break;
        case SIGUSR1: Request(REQ_SNAPSHOT); break;
        case SIGUSR2: Request(REQ_CLOSE); break;
        default: break;
        }
    }
    return NULL;
}

int SignalsStart(Line *l) {
    line = l;
    atomic_store(&quit, 0);
    if (ThreadCreate(&thread, SignalsMain, NULL, 0) != 0) {
        return -1;
    }
    started = 1;
    return 0;
}

void SignalsStop(void) {
    if (!started) {
        return;
    }
    atomic_store(&quit, 1);
    pthread_kill(thread, SIGUSR2);
    pthread_join(thread, NULL);
    started = 0;
}
