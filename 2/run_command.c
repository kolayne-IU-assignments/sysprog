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
#include <inttypes.h>

#include "parse_command.h"
#include "run_command.h"
#include "exit_status.h"

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
        // TODO: merge this with the other implementation of exit
        if (pc->argv[1] && pc->argv[2])
            die("exit must get no more than one argument\n");
        else if (pc->argv[1]) {
            char *inv;
            int code = strtoimax(pc->argv[1], &inv, 0);
            // Note: overflow/underflow errors are ignored
            if (*inv) {
                die("The argument to exit must be numeric\n");
            } else {
                exit(code);
            }
        } else {
            exit(EXIT_SUCCESS);
        }
    }
    execvp(pc->argv[0], pc->argv);
    die("Failed to exec %s: %s\n", pc->argv[0], strerror(errno));
}

/**
 * Returns `true` if `pc` was a special action, thus, no need to perform anything else.
 */
bool handle_special(const struct piped_commands *const pc) {
    if (pc->next)
        return false;

    if (!strcmp(pc->argv[0], "exit")) {
        int exit_code;
        if (pc->argv[1] && pc->argv[2]) {
            fprintf(stderr, "exit must get no more than one argument\n");
            exit_code = EXIT_FAILURE;
        } else if (pc->argv[1]) {
            char *inv;
            exit_code = strtoimax(pc->argv[1], &inv, 0);
            // Note: overflow/underflow errors are ignored
            if (*inv) {
                fprintf(stderr, "The argument to exit must be numeric\n");
                exit_code = EXIT_FAILURE;
            }
        } else {
            exit_code = EXIT_SUCCESS;
        }

        exit(exit_code);
        assert(false);
        // Would be
        // return true;
    } else if (!strcmp(pc->argv[0], "cd")) {
        // TODO: merge this with another implementation of `cd`
        if (pc->argv[1] != NULL && pc->argv[2] == NULL) {
            if (0 > chdir(pc->argv[1])) {
                perror("Failed to chdir");
            }
        } else {
            fprintf(stderr, "cd must get exatly one argument\n");
        }
        return true;
    }
    return false;
}


int process_sequenced_commands(struct sequenced_commands *const sc) {
    int exit_status = EXITSTATUS_DEFAULT;
    enum sequencing_type run_next = UNCONDITIONAL;

    struct sequenced_commands *sc_cur = sc;
    for(; sc_cur; sc_cur = sc_cur->next) {
        bool success = (WIFEXITED(exit_status) && 0 == WEXITSTATUS(exit_status));

        if ((success && run_next == SKIP_SUCCESS) ||
                (!success && run_next == SKIP_FAILURE))
            continue;

        if (handle_special(sc_cur->p_head)) {
            // `sc_cur->p_head` was a special command (e.g. cd or exit) - we performed
            // it in `handle_special`. Nothing else to do.
            continue;
        }

        run_next = sc_cur->run_next;

        // Children will write their pids into this pipe, I will wait for them.
        // It would not be safe to just do the correct number of `wait`s, as the
        // children (after `exec`) may create new siblings, which will turn to my
        // children, which would lead to a mess.
        //
        // Instead, before children `exec`, they write their pid to the stream,
        // I read it from here and reap them.
        int children_pids_pipe[2];
        int err = pipe(children_pids_pipe);
        if(err) {
            fprintf(stderr, "Failed to pipe: %s\n", strerror(errno));
            exit_status = EXITSTATUS_BEDA;
            goto handle_out;
        }

        pid_t res = fork();
        switch (res) {
            case 0:
                // Child
                process_piped_commands(sc_cur->p_head, children_pids_pipe[1]);
                // Won't return
                assert(false);
            case -1:
                fprintf(stderr, "Couldn't fork\n");
                exit_status = EXITSTATUS_BEDA;
                goto handle_out;
        }

        err = close(children_pids_pipe[1]);
        assert(!err);  // If failed to close, will self-deadlock below

        pid_t child;
        size_t readb;
        while (0 != (readb = read(children_pids_pipe[0], &child, sizeof child))) {
            assert(readb == sizeof child);  // Expect no errors to occur
            int res = waitpid(child, &exit_status, 0);
            assert(res > 0);
        }
        (void)close(children_pids_pipe[0]);
    }

handle_out:
    destroy_sequenced_commands(sc);

    return exit_status;
}
