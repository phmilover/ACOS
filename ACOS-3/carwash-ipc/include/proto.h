/**
 * @file proto.h
 * @brief Программа 4: обмен сообщениями через именованные каналы (FIFO).
 *
 * Разделяемой памяти нет. Состояние линии хранит только координатор; остальные процессы
 * присылают ему сообщения в общий канал /tmp/<key>/coord и получают ответы в свой канал
 * /tmp/<key>/p<pid>. Сообщение - структура фиксированного размера меньше PIPE_BUF (512 байт
 * на macOS), поэтому запись в общий канал от нескольких процессов атомарна и сообщения не
 * перемешиваются.
 *
 * Именованные семафоры: "/<key>.q<s>" - свободные места в накопителе стадии s (машина ждёт
 * на нём, держа пост, как в программах 1-3), "/<key>.open" - источник машин ждёт открытия
 * въезда.
 */
#ifndef PROTO_H
#define PROTO_H

#include <semaphore.h>

#include "config.h"

typedef enum { K_POST, K_CARS, K_CAR } Kind;

typedef enum {
    // участник -> координатор
    M_HELLO,      ///< регистрация: kind; пост name=стадия b=номер; машина a=ячейка
    M_ARRIVE,     ///< машина у въезда: a=программа, b=1 - взяла место в первой очереди
    M_BLOCKED,    ///< машина ждёт места в следующем накопителе, стоя на посту
    M_MOVE,       ///< машина переходит к следующей стадии маршрута
    M_LEAVE,      ///< машина прошла последнюю стадию и уезжает
    M_OPSTART,    ///< пост начал операцию: a=минут
    M_OPEND,      ///< пост закончил операцию: a=минут
    M_BREAK,      ///< пост сломался: a=минут ремонта
    M_REPAIR,     ///< пост починили
    M_IDLE,       ///< пост готов принять машину
    M_NEWCAR,     ///< источник просит ячейку для новой машины
    M_CLOSE,      ///< источник: машин больше не будет (a=сколько приехало)
    M_INTERRUPT,  ///< участник получил SIGINT/SIGTERM
    M_BYE,        ///< участник отключается
    // координатор -> участник
    M_WELCOME,  ///< ответ на HELLO, за ним следует Config
    M_SLOT,     ///< источнику: a=ячейка (-1 - въезд закрыт), b=номер машины
    M_SERVE,    ///< посту: прими машину из ячейки a (номер b)
    M_FREE,     ///< посту: машина съехала
    M_DONE,     ///< машине: операция над тобой закончена
    M_STOP,     ///< всем: имитация остановлена
} MsgType;

typedef struct {
    int type;  ///< MsgType
    int kind;  ///< Kind отправителя
    int pid;   ///< процесс отправителя
    int a, b;
    char name[NAME_LEN];  ///< имя стадии в M_HELLO поста
} Msg;

/// Ответ на M_HELLO: заголовок и конфигурация линии.
typedef struct {
    Msg head;
    Config cfg;
} Welcome;

/// Канал участника.
typedef struct {
    int coord;  ///< запись в общий канал координатора
    int mine;   ///< чтение из своего канала
    char path[64];
} Conn;

/// "/tmp/<key>" и "/tmp/<key>/coord".
void ProtoDir(const char *key, char *buf, int size);
void ProtoCoordPath(const char *key, char *buf, int size);
/// "/<key>.q<s>" и "/<key>.open".
void ProtoSemName(const char *key, int stage, char *buf, int size);

/**
 * @brief Участник создаёт свой канал, регистрируется у координатора и получает конфигурацию.
 *
 * Координатор мог ещё не запуститься - попытки идут до @p seconds секунд.
 * @return 0 или -1 (сообщение в stderr)
 */
int ConnOpen(Conn *c, const char *key, Kind kind, int a, int b, const char *name, Config *cfg,
             int seconds, const char *prog);
/// Отправляет сообщение координатору. @return 0 или -1 (координатора нет)
int ConnSend(Conn *c, Kind kind, int type, int a, int b);
/**
 * @brief Ждёт сообщение в свой канал.
 * @param timeoutMs -1 - ждать сколько угодно
 * @return 1 - сообщение в @p m, 0 - время вышло, -1 - координатор пропал, -2 - пришёл сигнал
 */
int ConnRecv(Conn *c, Msg *m, int timeoutMs);
/// Закрывает каналы и удаляет свой FIFO.
void ConnClose(Conn *c);

/// Открывает семафоры накопителей и семафор открытия въезда. @return 0 или -1
int ProtoOpenSems(const char *key, const Config *cfg, sem_t **queue, sem_t **open);

#endif
