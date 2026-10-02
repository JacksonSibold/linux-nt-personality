#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/types.h>
int main(int argc, char **argv) {
    if (argc < 2) return 1;
    pid_t pid = fork();
    if (pid==0) {
        execl(argv[1], argv[1], NULL);
        perror("execl");
        _exit(127);
    } else if (pid>0) {
        int status;
        pid_t w = waitpid(pid, &status, 0);
        printf("waitpid %d status 0x%x raw %d\n", w, status, status);
        printf("WIFEXITED %d WEXITSTATUS %d WIFSIGNALED %d WTERMSIG %d WIFSTOPPED %d WSTOPSIG %d\n",
            WIFEXITED(status), WIFEXITED(status)?WEXITSTATUS(status):-1,
            WIFSIGNALED(status), WIFSIGNALED(status)?WTERMSIG(status):-1,
            WIFSTOPPED(status), WIFSTOPPED(status)?WSTOPSIG(status):-1);
        if (WIFEXITED(status)) return WEXITSTATUS(status);
        if (WIFSIGNALED(status)) return 128+WTERMSIG(status);
        return 1;
    } else {
        perror("fork");
        return 1;
    }
}
