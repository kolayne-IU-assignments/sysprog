#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <assert.h>
#include <unistd.h>
#include <sys/wait.h>
#include <unistd.h>
#include <errno.h>

#include "parse_command.h"
#include "run_command.h"


void unwrap_p(const struct piped_commands *pc) {
    printf("  argc : %d\n", pc->_argc);
    printf("  outfile : %s\n", pc->outfile);
    printf("  append : %d\n", pc->append);
    printf("  argv : \n");
    for (char **s = pc->argv; *s; ++s) {
        printf("    %s\n", *s);
    }
    if (pc->next) {
        printf(" |\n");
        unwrap_p(pc->next);
    }
}

void unwrap_s(struct sequenced_commands *sc) {
    printf("/\n");
    unwrap_p(sc->p_head);
}


// `acc` parameter is just a hack to enforce tail recursion. The outer caller shall specify `0`.
__attribute__((pure)) size_t count_pipes(const struct piped_commands *const pc, int acc) {
    if(!pc)
        return acc;
    return count_pipes(pc->next, acc+1);
}


int main() {
    char cmd[] = "echo \\\"123 \"a\"\"b\" |grep    123| grep 456|grep  789  | cat >> f | echo \"\" \"gh\" i\"j\"k>l";
    //printf("Original string: %s\n", cmd);

    struct parse_result p = parse_command_line(cmd);
    assert(!p.err);
    //unwrap_s(&p.s_head);
    destroy_sequenced_commands(&p.s_head);

    //puts("\n");

    char *s;
    while (EOF != scanf(" %m[^\n]", &s)) {
        struct parse_result p = parse_command_line(s);
        if (p.err) {
            printf(": %s\n", p.err);
        } else {
            const size_t children_count = count_pipes(p.s_head.p_head, 0);

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
                continue;
            }

            pid_t res = fork();
            switch (res) {
                case 0:
                    // Child
                    process_piped_commands(p.s_head.p_head, children_pids_pipe[1]);
                    // Won't return
                    assert(false);
                case -1:
                    fprintf(stderr, "Couldn't fork\n");
                    continue;
            }

            err = close(children_pids_pipe[1]);
            assert(!err);  // If failed to close, will self-deadlock below

            pid_t child;
            size_t readb;
            while (0 != (readb = read(children_pids_pipe[0], &child, sizeof child))) {
                assert(readb == sizeof child);  // Expect no errors to occur
                int res = waitpid(child, NULL, 0);
                assert(res > 0);
            }
            (void)close(children_pids_pipe[0]);

            destroy_sequenced_commands(&p.s_head);
        }
        free(s);
    }
}
