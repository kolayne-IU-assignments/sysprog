#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <assert.h>

#include "parse_command.h"

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

    char *s;
    while (EOF != scanf(" %m[^\n]", &s)) {
        struct parse_result p = parse_command_line(s);
        if (p.err) {
            printf(": %s\n", p.err);
        } else {
            unwrap_s(&p.s_head);
            destroy_sequenced_commands(&p.s_head);
        }
        free(s);
    }
}
