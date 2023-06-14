#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include "tokenizer.c"
#include "parse_command.c"

int main() {
    char cmd[] = "echo \\\"123 \"a\"\"b\" |grep    123| grep 456|grep  789  | cat >> f | echo \"\" \"gh\" i\"j\"k>l";
    printf("Original string: %s\n", cmd);

    struct sequenced_commands cmds = parse_command_line(cmd);

    /*char *future_pointers[255];
    int fut_put = 0;

    char color[strlen(cmd) + 1];
    if (escape_and_color(cmd, color)) {
        // Stage 1: find them all
        int now = 0;
        while(1) {
            now += whitespace_advance(cmd + now);

            int read = next_token(cmd + now);
            if (read <= 0) {
                printf("next_token returned %d\n", read);
                break;
            }
            printf("Next token: %.*s\n", read, cmd + now);
            if (!is_word_separator(cmd[now]))
                future_pointers[fut_put++] = cmd + now;
            now += read;
        }

        // Stage 2: terminate strings
        now = 0;
        while(1) {
            now += whitespace_advance(cmd + now);

            int read = next_token(cmd + now);
            if (read <= 0) {
                break;
            }
            int old = now;
            now += read;
            if (!is_word_separator(cmd[old]))
                cmd[now++] = '\0';
        }

        // Stage 3: check that the arguments are correct
        puts("\nNow cmd-line arguments only:");
        for (int i = 0; i < fut_put; ++i) {
            printf("%s ", future_pointers[i]);
        }
        puts("");
    } else {
        puts("\\ at the end of input");
    }*/
}
