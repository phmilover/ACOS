/**
 * @file render.c
 * @brief Поток отрисовки: монитор «ограниченный буфер» на два кадра.
 *
 * Буферы идут по кругу 0, 1, 0, ...: Submit пишет в один, пока поток рисует другой. count -
 * сколько буферов занято кадром, включая тот, что сейчас рисуется: буфер возвращается только
 * после отрисовки, поэтому Submit его не затрёт.
 */

#include "render.h"

#include <pthread.h>
#include <stdatomic.h>
#include <string.h>

#include "sync.h"
#include "thread.h"
#include "view_live.h"

static Mutex lock;  // RANK_RENDER: защищает всё ниже, кроме содержимого рисуемого буфера
static Cond notFull, notEmpty, drained;
static Snapshot buffer[2];
static int head GUARDED_BY(lock) = 0;      // какой буфер рисовать следующим
static int count GUARDED_BY(lock) = 0;     // занятые буферы (0..2)
static int stopping GUARDED_BY(lock) = 0;  // RenderStop зовёт: дорисовать и выйти
static atomic_int active = 0;
static pthread_t thread;

static void *RenderMain(void *unused) {
    (void)unused;
    LockdepSetThreadName("render");
    MutexLock(&lock);
    for (;;) {
        while (count == 0 && !stopping) {
            CondWait(&notEmpty, &lock);
        }
        if (count == 0) {
            break;  // остановка, а кадры, отданные до неё, уже нарисованы
        }
        int slot = head;
        MutexUnlock(&lock);
        ViewLiveDraw(
            &buffer[slot]);  // без мьютекса: Submit в это время кладёт кадр в соседний буфер
        MutexLock(&lock);
        head = 1 - head;
        --count;
        CondSignal(&notFull);
        if (count == 0) {
            CondBroadcast(&drained);
        }
    }
    MutexUnlock(&lock);
    return NULL;
}

int RenderStart(void) {
    if (atomic_load(&active)) {
        return 0;
    }
    if (CondInit(&notFull) != 0) {
        return -1;
    }
    if (CondInit(&notEmpty) != 0) {
        CondDestroy(&notFull);
        return -1;
    }
    if (CondInit(&drained) != 0) {
        CondDestroy(&notEmpty);
        CondDestroy(&notFull);
        return -1;
    }
    MutexInit(&lock, RANK_RENDER, "render");
    MutexLock(&lock);
    head = 0;
    count = 0;
    stopping = 0;
    MutexUnlock(&lock);
    if (ThreadCreate(&thread, RenderMain, NULL, 0) != 0) {
        MutexDestroy(&lock);
        CondDestroy(&drained);
        CondDestroy(&notEmpty);
        CondDestroy(&notFull);
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
    MutexLock(&lock);
    while (count == 2) {
        CondWait(&notFull, &lock);  // оба буфера заняты: ждём, пока нарисуют более ранний кадр
    }
    memcpy(&buffer[(head + count) % 2], s, sizeof *s);
    ++count;
    CondSignal(&notEmpty);
    MutexUnlock(&lock);
}

void RenderFlush(void) {
    if (!atomic_load(&active)) {
        return;
    }
    MutexLock(&lock);
    while (count > 0) {
        CondWait(&drained, &lock);
    }
    MutexUnlock(&lock);
}

void RenderStop(void) {
    if (!atomic_load(&active)) {
        return;
    }
    MutexLock(&lock);
    stopping = 1;
    CondSignal(&notEmpty);  // разбудить, если ждёт кадр
    MutexUnlock(&lock);
    pthread_join(thread, NULL);
    MutexDestroy(&lock);
    CondDestroy(&drained);
    CondDestroy(&notEmpty);
    CondDestroy(&notFull);
    atomic_store(&active, 0);
}
