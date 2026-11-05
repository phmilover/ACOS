/**
 * @file net.h
 * @brief TCP: сокет сервера, подключение клиента и построчный обмен.
 *
 * Протокол текстовый: каждое сообщение - одна строка из слов через пробел, заканчивается '\n'
 * (полный список - в README, раздел «Протокол»). Текст удобен тем, что сообщения не зависят от
 * порядка байтов и размера int на разных машинах, а сервер можно проверить через nc.
 *
 * TCP - поток байтов: одно чтение может вернуть половину строки или несколько строк сразу.
 * LineConn копит прочитанное и отдаёт только целые строки.
 */
#ifndef NET_H
#define NET_H

#define LINE_MAX_LEN 512   ///< самая длинная строка протокола
#define NET_BUF      8192  ///< буфер приёма одного соединения

/// Соединение с буфером приёма.
typedef struct {
    int fd;  ///< сокет или -1
    char buf[NET_BUF];
    int len;  ///< сколько байт в buf ещё не разобрано
} LineConn;

/**
 * @brief Сокет сервера: bind на @p ip:@p port и listen.
 * @param ip адрес интерфейса ("0.0.0.0" - все интерфейсы)
 * @return дескриптор или -1, причина - в @p err
 */
int NetListen(const char *ip, int port, char *err, int errSize);

/**
 * @brief Подключение к серверу @p host:@p port (имя или IP).
 *
 * Сервер мог ещё не запуститься, поэтому попытки повторяются каждые 300 мс до @p seconds
 * секунд. Ctrl+C прерывает ожидание.
 * @return дескриптор или -1, причина - в @p err
 */
int NetConnect(const char *host, int port, int seconds, char *err, int errSize);

/// "192.168.1.5:51234" - адрес другой стороны соединения.
void NetPeerName(int fd, char *buf, int size);

void LineInit(LineConn *c, int fd);
void LineClose(LineConn *c);

/**
 * @brief Один read из сокета в буфер.
 * @return >0 - прочитано байт, 0 - другая сторона закрыла соединение, -1 - ошибка или
 *         переполнение (строка длиннее LINE_MAX_LEN), -2 - прервано сигналом
 */
int LineFill(LineConn *c);

/// Забирает из буфера следующую целую строку без '\n'. @return 1 или 0 (целой строки нет)
int LineNext(LineConn *c, char *out, int size);

/**
 * @brief Ждёт следующую строку не дольше @p timeoutMs (-1 - сколько угодно).
 * @return 1 - строка в @p out, 0 - время вышло, -1 - соединение закрыто, -2 - пришёл сигнал
 */
int LineWait(LineConn *c, char *out, int size, int timeoutMs);

/// Отправляет одну строку протокола ('\n' добавляется). @return 0 или -1
int NetSend(int fd, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

/// Разбирает номер порта 1..65535. @return 0 или -1 (сообщение в stderr)
int NetParsePort(const char *s, int *port);

#endif
