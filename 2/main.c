#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <assert.h>
#include <unistd.h>
#include <sys/wait.h>
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

int main() {
    char cmd[] = "echo \\\"123 \"a\"\"b\" |grep    123| grep 456|grep  789  | cat >> f | echo \"\" \"gh\" i\"j\"k>l";
    printf("Original string: %s\n", cmd);

    struct parse_result p = parse_command_line(cmd);
    assert(!p.err);
    unwrap_s(&p.s_head);
    destroy_sequenced_commands(&p.s_head);

    puts("\n");

    // Make sure I don't have any children yet (for example if my parent made children and exec'ed me)
    if (0 == waitpid(-1, NULL, WNOHANG) || errno != ECHILD) {
        fprintf(stderr, "I already have children who I have not born. Refusing to continue\n");
        return EXIT_FAILURE;
    }

    char *s;
    while (EOF != scanf(" %m[^\n]", &s)) {
        struct parse_result p = parse_command_line(s);
        if (p.err) {
            printf(": %s\n", p.err);
        } else {
            switch (fork()) {
                case 0:
                    process_piped_commands(p.s_head.p_head);
                    // Won't return
                    assert(false);
                case -1:
                    fprintf(stderr, "Couldn't fork\n");
                    continue;
                default:
                    // Wait for all children to terminate.
                    // If all children were successfully created then I will reap them all;
                    // If some of them failed to clone, the excessive `wait`s will fail silently.
                    for(struct piped_commands *ps = p.s_head.p_head; ps; ps = ps->next) {
                        int res = wait(NULL);
                        assert(res == 0 || errno == ECHILD);
                    }
            }

            destroy_sequenced_commands(&p.s_head);
        }
        free(s);
    }
}
