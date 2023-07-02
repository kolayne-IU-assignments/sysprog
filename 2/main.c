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
#include "errors.h"


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


/**
 * Returns `true` if `pc` was a special action, thus, no need to perform anything else.
 */
bool handle_special(const struct piped_commands *const pc) {
    if (pc->next)
        return false;

    if (!strcmp(pc->argv[0], "exit")) {
        fclose(stdin);
        return true;
    } else if (!strcmp(pc->argv[0], "cd")) {
        // TODO: merge this with another implementation of `cd`
        if (pc->argv[1] != NULL && pc->argv[2] == NULL) {
            if (0 > chdir(pc->argv[1])) {
                perror("Failed to chdir");
            }
        } else {
            fprintf(stderr, "cd requires exatly one argument\n");
        }
        return true;
    }
    return false;
}


/**
 * `sza_excl` shall not include the null terminator (i.e. be equal to `strlen`);
 * `szb_incl` shall include the null terminator (i.e. be `strlen` + 1).
 *
 * `a` must be either a pointer to a `malloc`-allocated string, or `NULL` (in the
 * latter case `sza_excl` shall be zero). It will be reallocated (the previous pointer
 * is invalidated), unless an error.
 *
 * `b` must be a pointer to a string (allocation does not matter, `b` is not freed).
 */
char *realloc_append(char *a, const char *b, size_t sza_excl, size_t szb_incl) {
    char *res = realloc(a, sza_excl + szb_incl);
    if (res == NULL)
        return NULL;
    memcpy(res + sza_excl, b, szb_incl);
    return res;
}

struct parse_result read_and_parse_command_line(char **to_free) {
    /*
     * Just like the string coloring algorithm, this implementation
     * gives the performance that is far from optimal but results in
     * a simple and easy to read code.
     */

    struct parse_result res = {};
    char *prev = NULL;
    size_t prev_len = 0;

    // Keep reading lines until a complete command is consumed (or the input is over)
    while (1) {
        char *s;
        int sres = scanf("%m[^\n]", &s);

        if (sres == 0) {
            // Scanf didn't read anything but it's not a `EOF`, therefore, it's a
            // newline character.

            char c = getc(stdin);
            assert(c == '\n');

            if (prev[prev_len] == '\\') {
                // If a backslash symbol follows the string instead of a NULL terminator,
                // it means the last string finished with a backslash, thus the newline
                // symbol should not be stored as pasrt of the command, so, do nothing.
            } else {
                // Otherwise, we're inside a quotation right now, so the newline character
                // should be preserved.
                prev = realloc_append(prev, "\n", prev_len, 2);
                ++prev_len;
            }

            continue;
        } else if (sres == EOF) {
            // The input is over. If it failed on the first iteration (empty `prev`),
            // indicate that the input is over, otherwise return the previous results
            // (below).
            if (!prev)
                res.err = err_input_is_over;
        } else {
            // Successful input. Combine with the previous data and attempt to parse.

            if (s[0] == '#') {
                free(s);
                // Consume the newline character here to not confuse the parser with it
                char c = getc(stdin);
                assert(c == '\n');
                continue;
            }

            size_t s_len = strlen(s);
            char *combined = realloc_append(prev, s, prev_len, s_len + 1);
            free(s);
            if (combined == NULL) {
                // Out of memory. Indicate that
                res.err = err_oom;
            } else {
                prev = combined;
                prev_len += s_len;

                res = parse_command_line(prev);

                // Note: here it is safe to compare strings as pointers because `res.err`
                // may only be assigned to a global constant defined in errors.c

                if (res.err == err_trailing_backslash || res.err == err_unclosed_quot) {
                    if (res.err == err_trailing_backslash) {
                        // Remove backslash from the final command
                        --prev_len;
                    }

                    continue;
                }
            }
        }

        // If reached here, no need to continue reading.
        break;
    }

    // Either failed to `scanf` this time (so return results of the previous iteration),
    // or an error (other than quotation/backslash thing) occurred this time (so return it),
    // or everything successful! (so return it).
    *to_free = prev;
    return res;
}

int main() {
    char cmd[] = "echo \\\"123 \"a\"\"b\" |grep    123| grep 456|grep  789  | cat >> f | echo \"\" \"gh\" i\"j\"k>l";
    //printf("Original string: %s\n", cmd);

    struct parse_result p = parse_command_line(cmd);
    assert(!p.err);
    //unwrap_s(&p.s_head);
    destroy_sequenced_commands(&p.s_head);

    //puts("\n");

    while (1) {
        (void)scanf(" ");  // Skip whitespace between commands
        char *to_free;
        struct parse_result p = read_and_parse_command_line(&to_free);
        if (p.err == err_input_is_over) {
            break;
        } else if (p.err) {
            printf(": %s\n", p.err);
        } else {
            if (handle_special(p.s_head.p_head))
                goto handle_out;

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
                goto handle_out;
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
                    goto handle_out;
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

handle_out:
            destroy_sequenced_commands(&p.s_head);
        }

        // The consumed input string must be freed regardless of parsing success
        free(to_free);
    }
}
