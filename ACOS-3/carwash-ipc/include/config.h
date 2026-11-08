/**
 * @file config.h
 * @brief Параметры модели и их разбор.
 *
 * Параметры читаются из файла вида "ключ = значение" (ключ -c), а если файла нет - из
 * встроенной конфигурации CONFIG_BUILTIN. Структура Config не содержит указателей: её целиком
 * кладут в разделяемую память и пересылают по каналу.
 */
#ifndef CONFIG_H
#define CONFIG_H

#define MAX_STAGES   6
#define MAX_POSTS    4
#define MAX_PROGRAMS 6
#define MAX_QUEUE    16  ///< вместимость накопителя из конфига строго меньше
#define NAME_LEN     12
#define PATH_LEN     256

/// Кого первым забирать из очереди.
typedef enum {
    PRIO_FIFO,      ///< кто раньше встал в очередь
    PRIO_PROGRAM,   ///< у чьей программы приоритет выше
    PRIO_SHORTEST,  ///< кому осталось меньше стадий
} PriorityRule;

/// Стадия мойки: строка "stage = имя посты tmin tmax очередь".
typedef struct {
    char name[NAME_LEN];
    int posts;       ///< 0 постов допустимо: программы через такую стадию невыполнимы
    int tmin, tmax;  ///< время операции, минут
    int queue;       ///< вместимость накопителя перед стадией (0 - машина ждёт на посту)
} StageSpec;

/// Программа обслуживания: маршрут по стадиям в порядке линии.
typedef struct {
    char name[NAME_LEN];
    int weight;  ///< как часто клиенты выбирают программу
    int priority;
    int route[MAX_STAGES];
    int len;
} ProgramSpec;

typedef struct {
    StageSpec stage[MAX_STAGES];
    int stages;
    ProgramSpec program[MAX_PROGRAMS];
    int programs;

    int cars;  ///< сколько машин приедет, 0 - без ограничения
    int arrMin, arrMax;
    int day;       ///< сколько минут открыт въезд, 0 - без ограничения
    double failP;  ///< вероятность поломки поста после каждой операции
    int repairMin, repairMax;
    PriorityRule rule;

    unsigned seed;
    int delayMs;  ///< сколько миллисекунд длится одна минута модели
    int quiet;    ///< ключ -q: события только в журнал, на экран - итоги
    char journal[PATH_LEN];
} Config;

/// Конфигурация по умолчанию: обычный день, четыре стадии, три программы.
extern const char *CONFIG_BUILTIN;

void ConfigDefaults(Config *c);
/// @return 0 или -1, причина ошибки - в @p err
int ConfigParseText(Config *c, const char *text, char *err, int errSize);
/// @return 0 или -1, причина ошибки - в @p err
int ConfigLoadFile(Config *c, const char *path, char *err, int errSize);
/// Проверяет, что параметры не противоречат друг другу. @return 0 или -1
int ConfigValidate(const Config *c, char *err, int errSize);
void ConfigPrint(const Config *c, int fd);
const char *ConfigRuleName(PriorityRule rule);

/// Стадия маршрута программы, на которой нет ни одного поста, или -1.
int ConfigDeadStage(const Config *c, int program);
/// Сколько программ можно выполнить (во всех стадиях маршрута есть посты).
int ConfigPossiblePrograms(const Config *c);
/// Общее число постов всех стадий.
int ConfigTotalPosts(const Config *c);

/// @return 0 и число в *out, или -1, если строка - не целое число
int ParseInt(const char *s, int *out);

/**
 * @brief Собирает конфигурацию из командной строки.
 *
 * Общие ключи: -c FILE, -s SEED, -d MS, -l FILE, -u, -q. Сначала читается файл из -c (или
 * встроенная конфигурация), затем поверх него применяются остальные ключи, поэтому их порядок
 * не важен. Ключи из @p extra (строка в формате getopt) передаются в @p onExtra.
 * @return 0 или -1 (сообщение об ошибке уже в stderr)
 */
int ConfigFromArgs(Config *c, int argc, char **argv, const char *extra,
                   int (*onExtra)(int opt, const char *arg));

#endif
