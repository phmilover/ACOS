/*
 * Программа 3, пост мойки: отдельный процесс на каждый пост. Подключается к координатору по
 * ключу, занимает свой пост и обслуживает машины, пока имитация не остановится.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "model.h"
#include "util.h"

int main(int argc, char **argv) {
    const char *key = "cw3";
    int opt;
    while ((opt = getopt(argc, argv, "k:h")) != -1) {
        if (opt == 'k') {
            key = optarg;
        } else {
            dprintf(opt == 'h' ? 1 : 2, "Usage: cw3-post [-k KEY] STAGE NUMBER\n"
                                        "  STAGE  stage name from the config, e.g. tunnel\n"
                                        "  NUMBER post number in the stage, from 1\n");
            return opt == 'h' ? 0 : 1;
        }
    }
    if (argc - optind != 2) {
        dprintf(2, "Usage: cw3-post [-k KEY] STAGE NUMBER\n");
        return 1;
    }
    SignalsInstall();
    Ipc ipc;
    if (MAttach(&ipc, key, 10, "cw3-post") != 0) {
        return 5;
    }
    State *st = &ipc.sh->st;
    const Config *cfg = &st->cfg;
    int stage = -1, number = 0;
    for (int s = 0; s < cfg->stages; ++s) {
        if (strcmp(cfg->stage[s].name, argv[optind]) == 0) {
            stage = s;
        }
    }
    if (stage < 0 || ParseInt(argv[optind + 1], &number) != 0 || number < 1 ||
        number > cfg->stage[stage].posts) {
        dprintf(2, "cw3-post: no post '%s %s' in this configuration\n", argv[optind],
                argv[optind + 1]);
        IpcDetach(&ipc);
        return 1;
    }
    if (MOpenJournal(&ipc) != 0) {
        IpcDetach(&ipc);
        return 5;
    }

    // пост занимаем под мьютексом: второй процесс с тем же номером получит отказ
    MLock(&ipc);
    PostSlot *me = &st->post[stage][number - 1];
    int busy = me->state != POST_OFF;
    if (!busy) {
        me->state = POST_IDLE;
        me->pid = getpid();
    }
    MUnlock(&ipc);
    if (busy) {
        dprintf(2, "cw3-post: post %s.%d is already run by pid %d\n", cfg->stage[stage].name,
                number, me->pid);
        IpcDetach(&ipc);
        return 1;
    }
    dprintf(2, "cw3-post: post %s.%d connected (pid %d)\n", cfg->stage[stage].name, number,
            (int)getpid());
    int code = MPost(&ipc, stage, number - 1);
    IpcDetach(&ipc);
    return code;
}
