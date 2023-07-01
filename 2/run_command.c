#include <fcntl.h>
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <unistd.h>
#include <string.h>
#include <assert.h>
#include <stdarg.h>
#include <linux/sched.h>
#include <sys/syscall.h>

#include "parse_command.h"
#include "run_command.h"

__attribute__((noreturn)) void die(const char *fmt, ...) {
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

/**
 * Run commands with output piped into each other, with last one possibly redirected to
 * a file.
 *
 * `pc` points to the linked list of `struct piped_commands` to be executed.
 *
 * `write_my_pid_fd` must be a writable file descriptor, where the pids of the
 * current process and all created siblings will be written (as a binary sequence of `pid_t`
 * values, no padding).
 */
__attribute__((noreturn)) void process_piped_commands(const struct piped_commands *const pc,
                                                      int write_my_pid_fd) {
    // Let the big brother know I should be reaped.
    pid_t self = getpid();
    size_t written = write(write_my_pid_fd, &self, sizeof self);
    assert(written == sizeof self);
    
    if(pc->next) {
        int fildes[2];
        if (0 > pipe(fildes)) {
            die("Failed to open pipe: %s\n", strerror(errno));
        }

        pid_t pid = sibling_fork();
        if (0 > pid) {
            die("Failed to clone3 from %s: %s\n", pc->argv[0], strerror(errno));
        } else if (0 == pid) {
            // Child
            dup2(fildes[0], STDIN_FILENO);
            close(fildes[0]);
            close(fildes[1]);
            process_piped_commands(pc->next, write_my_pid_fd);
            // `process_piped_commands` always either terminates or `exec`s
            assert(false);
        } else {
            // Parent
            (void)close(write_my_pid_fd);
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
                die("Failed to dup2 for %s: %s\n", pc->argv[0], strerror(errno));
            }
            (void)close(fd);
        }
    }

    if (!strcmp(pc->argv[0], "cd")) {
        if (pc->argv[1] != NULL || pc->argv[2] == NULL) {
            if (0 > chdir(pc->argv[1])) {
                die("Failed to chdir to %s: %s\n", pc->argv[1], strerror(errno));
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
