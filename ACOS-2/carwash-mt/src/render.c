/**
 * @file render.c
 * @brief Поток отрисовки: ждёт кадр на ready, рисует, возвращает буфер на free.
 */

#include "render.h"

#include <pthread.h>
#include <stdatomic.h>
#include <string.h>

#include "mutex.h"
#include "sem.h"
#include "view_live.h"

static Snapshot buffer[2];
static int submitSlot = 0;       // в какой буфер кладёт Submit; только вызывающий Submit
static atomic_int drawSlot = 0;  // какой буфер рисовать (Submit пишет до SemPost(ready))
static atomic_int pending = 0;   // 1 - на ready лежит кадр, 0 - ready разбудили ради остановки
static atomic_int stopping = 0;
static atomic_int active = 0;
static Sem freeSem, readySem;
static pthread_t thread;

static void *RenderMain(void *unused) {
    (void)unused;
    LockdepSetThreadName("render");
    for (;;) {
        SemWait(&readySem);
        if (atomic_exchange(&pending, 0)) {
            ViewLiveDraw(&buffer[atomic_load(&drawSlot)]);
            SemPost(&freeSem);
        }
        if (atomic_load(&stopping)) {
            return NULL;  // кадр, отданный до остановки, уже нарисован
        }
    }
}

int RenderStart(void) {
    if (atomic_load(&active)) {
        return 0;
    }
    if (SemOpen(&freeSem, 1) != 0) {
        return -1;
    }
    if (SemOpen(&readySem, 0) != 0) {
        SemClose(&freeSem);
        return -1;
    }
    atomic_store(&stopping, 0);
    atomic_store(&pending, 0);
    submitSlot = 0;
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
    SemWait(&freeSem);
    submitSlot = 1 - submitSlot;
    memcpy(&buffer[submitSlot], s, sizeof *s);
    atomic_store(&drawSlot, submitSlot);
    atomic_store(&pending, 1);
    SemPost(&readySem);
}

void RenderFlush(void) {
    if (!atomic_load(&active)) {
        return;
    }
    SemWait(&freeSem);  // free возвращается после отрисовки
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
