#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include "parser.c"

size_t whitespace_advance(const char *s) {
    size_t res = 0;
    for(; isspace(s[res]); ++res);
    return res;
}

int main() {
    char cmd[] = "echo \\\"123 \"a\"\"b\" |grep 123| grep 456|grep 789 | cat >> f | echo \"\" \"gh\" i\"j\"k>l";
    printf("Original string: %s\n", cmd);

    char color[strlen(cmd) + 1];
    if (escape_and_color(cmd, color)) {
        int now = 0;
        while(1) {
            now += whitespace_advance(cmd + now);

            int read = next_token(cmd + now);
            if (read <= 0) {
                printf("next_token returned %d\n", read);
                break;
            }
            printf("Next token: %.*s\n", read, cmd + now);
            now += read;
        }
    } else {
        puts("\\ at the end of input");
    }
}
