#include <errno.h>
#include <process.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>

#ifndef QNX_TEST_ARG
#define QNX_TEST_ARG ""
#endif

int main(void) {
    char *const argv_no_arg[] = { "/proc/boot/test-bin", NULL };
    char *const argv_with_arg[] = { "/proc/boot/test-bin", QNX_TEST_ARG, NULL };
    char *const *argv = QNX_TEST_ARG[0] ? argv_with_arg : argv_no_arg;
    int status;
    int result;

    errno = 0;
    status = spawnv(P_WAIT, argv[0], argv);
    if (status == -1) {
        fprintf(stderr, "__QNX_TEST_SPAWN_ERROR__=%d:%s\n", errno, strerror(errno));
        fflush(stderr);
        return 125;
    }

    if (WIFEXITED(status)) {
        result = WEXITSTATUS(status);
    } else if (WIFSIGNALED(status)) {
        int signal_number = WTERMSIG(status);
        printf("__QNX_TEST_SIGNAL__=%d\n", signal_number);
        result = 128 + signal_number;
    } else {
        result = 125;
    }

    printf("__QNX_TEST_RC__=%d\n", result);
    fflush(stdout);
    return 0;
}
