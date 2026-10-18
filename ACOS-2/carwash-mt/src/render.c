/**
 * @file render.c
 * @brief Поток отрисовки: ждёт кадр на ready, рисует, возвращает буфер на free.
 *
 * Буферы идут по кругу 0, 1, 0, ...: Submit пишет в один, пока поток рисует другой.
 */

#include "render.h"

#include <pthread.h>
#include <stdatomic.h>
#include <string.h>

#include "mutex.h"
#include "sem.h"
#include "view_live.h"

static Snapshot buffer[2];
static int submitSlot = 0;     // в какой буфер кладёт Submit; только вызывающий Submit
static int drawSlot = 0;       // какой буфер рисовать следующим; только поток отрисовки
static atomic_int filled = 0;  // сколько буферов с кадром ждут отрисовки (0..2)
static atomic_int stopping = 0;
static atomic_int active = 0;
static Sem freeSem, readySem;  // free - пустые буферы (начально 2), ready - заполненные (0)
static pthread_t thread;

static void *RenderMain(void *unused) {
    (void)unused;
    LockdepSetThreadName("render");
    for (;;) {
        SemWait(&readySem);
        // ready будят и ради остановки, тогда кадра может не быть
        if (atomic_load(&filled) > 0) {
            ViewLiveDraw(&buffer[drawSlot]);
            drawSlot ^= 1;
            atomic_fetch_sub(&filled, 1);
            SemPost(&freeSem);
        }
        if (atomic_load(&stopping) && atomic_load(&filled) == 0) {
            return NULL;  // кадры, отданные до остановки, уже нарисованы
        }
    }
}

int RenderStart(void) {
    if (atomic_load(&active)) {
        return 0;
    }
    if (SemOpen(&freeSem, 2) != 0) {
        return -1;
    }
    if (SemOpen(&readySem, 0) != 0) {
        SemClose(&freeSem);
        return -1;
    }
    atomic_store(&stopping, 0);
    atomic_store(&filled, 0);
    submitSlot = drawSlot = 0;
    if (pthread_create(&thread, NULL, RenderMain, NULL) != 0) {
        SemClose(&readySem);
        SemClose(&freeSem);
        return -1;
    }
    atomic_store(&active, 1);
    return 0;
}

int RenderActive(void) {
    return atomic_load(&active);
}

void RenderSubmit(const Snapshot *s) {
    if (!atomic_load(&active)) {
        return;
    }
    SemWait(&freeSem);  // буфер submitSlot свободен: его кадр уже нарисован
    memcpy(&buffer[submitSlot], s, sizeof *s);
    submitSlot ^= 1;
    atomic_fetch_add(&filled, 1);  // до SemPost: поток отрисовки увидит кадр
    SemPost(&readySem);
}

void RenderFlush(void) {
    if (!atomic_load(&active)) {
        return;
    }
    // оба буфера вернулись на free - значит, все отданные кадры нарисованы
    SemWait(&freeSem);
    SemWait(&freeSem);
    SemPost(&freeSem);
    SemPost(&freeSem);
}

void RenderStop(void) {
    if (!atomic_load(&active)) {
        return;
    }
    atomic_store(&stopping, 1);
    SemPost(&readySem);  // разбудить, если ждёт кадр
    pthread_join(thread, NULL);
    SemClose(&readySem);
    SemClose(&freeSem);
    atomic_store(&active, 0);
}
