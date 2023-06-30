#include <fcntl.h>
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <unistd.h>
#include <string.h>
#include <assert.h>
#include <stdarg.h>
#include <linux/sched.h>
#include <signal.h>
#include <sys/syscall.h>

#include "parse_command.h"
#include "run_command.h"

void die(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    va_end(args);
    exit(EXIT_FAILURE);
}

pid_t sibling_fork() {
    struct clone_args args = {
        .flags = CLONE_PARENT
    };

    return (pid_t)syscall(SYS_clone3, &args, sizeof args);
}

void process_piped_commands(const struct piped_commands *const pc) {
    if(pc->next) {
        int fildes[2];
        if (0 > pipe(fildes)) {
            die("Failed to open pipe: %s\n", strerror(errno));
        }

        pid_t pid = sibling_fork();
        if (0 > pid) {
            die("Failed to clone3: %s\n", strerror(errno));
        } else if (0 == pid) {
            // Child
            dup2(fildes[0], STDIN_FILENO);
            close(fildes[0]);
            close(fildes[1]);
            process_piped_commands(pc->next);
            // `process_piped_commands` always either terminates or `exec`s
            assert(false);
        } else {
            // Parent
            dup2(fildes[1], STDOUT_FILENO);
            close(fildes[0]);
            close(fildes[1]);
        }
    } else if(pc->outfile) {
        int fd = open(pc->outfile, O_CREAT | (pc->append ? O_APPEND : O_TRUNC) | O_WRONLY,
                S_IRUSR|S_IWUSR|S_IRGRP|S_IROTH);
        if (fd < 0) {
            die("Failed to open file %s: %s\n", pc->outfile, strerror(errno));
        } else {
            if (0 > dup2(fd, STDOUT_FILENO)) {
                die("Failed to dup2: %s\n", strerror(errno));
            }
            (void)close(fd);
        }
    }

    if (!strcmp(pc->argv[0], "cd")) {
        if (pc->argv[1] != NULL || pc->argv[2] == NULL) {
            if (0 > chdir(pc->argv[1])) {
                die("Failed to chdir: %s\n", strerror(errno));
            }
            // cd: nothing else to do
            exit(EXIT_SUCCESS);
        } else {
            die("cd must get exactly one argument\n");
        }
    } else if (!strcmp(pc->argv[0], "exit")) {
        // Note: ignoring arguments
        exit(EXIT_SUCCESS);
    }
    execvp(pc->argv[0], pc->argv);
    die("Failed to exec %s: %s\n", pc->argv[0], strerror(errno));
}
