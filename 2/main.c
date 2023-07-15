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
#include <inttypes.h>

#include "parse_command.h"
#include "run_command.h"
#include "errors.h"
#include "exit_status.h"


/**
 * `sza_excl` shall not include the null terminator (i.e. be equal to `strlen`);
 * `szb_incl` shall include the null terminator (i.e. be `strlen` + 1).
 *
 * `a` must be either a pointer to a `malloc`-allocated string, or `NULL` (in the
 * latter case `sza_excl` shall be zero). It will be reallocated (the previous pointer
 * is invalidated), unless an error.
 *
 * `b` must be a pointer to a string (allocation does not matter, `b` is not freed).
 *
 * `a` is freed even if reallocation failed.
 */
char *realloc_append_always_free(char *a, const char *b, size_t sza_excl, size_t szb_incl) {
    char *res = realloc(a, sza_excl + szb_incl);
    if (res == NULL) {
        free(a);
        return NULL;
    }
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

    // `prev_dup` stores the `strdup`ed version of `prev` that is fed into
    // `parse_command_line`. It shall not be freed before the corresponding
    // `struct parse_result` is destroyed.
    char *prev_dup = NULL;

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
                // it means the last input ended with a backslash, thus, the current newline
                // character should not be stored as pasrt of the command, so, do nothing.
                continue;
            } else {
                // Otherwise, we're inside a quotation right now, so the newline character
                // should be preserved.
                prev = realloc_append_always_free(prev, "\n", prev_len, 2);
                if (prev) {
                    ++prev_len;
                    continue;
                } else {
                    res.err = err_oom;
                    break;
                }
            }
        } else if (sres == EOF) {
            // The input is over. If it failed on the first iteration (empty `prev`),
            // indicate that the input is over, otherwise return the previous results
            // (below).
            if (!prev)
                res.err = err_input_is_over;
            break;
        } else {
            // Successful input. Combine with the previous data and attempt to parse.

            if (s[0] == '#' && res.err != err_unclosed_quot) {  // Looks like a comment
                free(s);
                // Consume the newline character here to not confuse the parser with it
                char c = getc(stdin);
                assert(c == '\n');
                continue;
            }

            size_t s_len = strlen(s);
            prev = realloc_append_always_free(prev, s, prev_len, s_len + 1);
            free(s);

            if (!prev || !(prev_dup = strdup(prev))) {
                // Out of memory. Report it.
                res.err = err_oom;
                break;
            } else {
                prev_len += s_len;
                res = parse_command_line(prev_dup);

                if (res.err) {
                    free(prev_dup);
                    prev_dup = NULL;

                    // Note: here it is safe to compare strings as pointers because `res.err`
                    // may only be assigned to a global constant defined in errors.c

                    if (res.err == err_trailing_backslash || res.err == err_unclosed_quot) {
                        if (res.err == err_trailing_backslash) {
                            // Remove backslash from the final command
                            --prev_len;
                        }

                        continue;
                    } else {
                        // If another error, report it.
                        break;
                    }
                } else {
                    /* Parsed successfully! Just exit the loop below. */
                    break;
                }
            }
        }

        // Unreachable: either a `continue` or a `break` in every
        // possible branch outcome above.
        assert(false);
    }

    // Either failed to `scanf` this time (so return results of the previous iteration),
    // or an error (other than quotation/backslash thing) occurred this time (so return it),
    // or everything successful! (so return it).
    free(prev);  // Note: `prev` is either a good thing that came from `realloc_append_always_free`,
                 // or `NULL` that came from `realloc_append_always_free`.
    *to_free = prev_dup;
    return res;
}


int main() {
    int exit_status = EXITSTATUS_DEFAULT;

    while (1) {
        (void)scanf(" ");  // Skip whitespace between commands
        if (feof(stdin)) {
            // In old enough libc the `scanf(" ")` is unable
            // do detect EOF which causes counter-intuitive behavior
            // in further attempts to scan input. Thus this separate
            // check: if the input has finished while attempting to
            // skip whitespace, break outright.
            break;
        }

        char *to_free;
        struct parse_result p = read_and_parse_command_line(&to_free);
        if (p.err == err_input_is_over) {
            break;
        } else if (p.err) {
            printf(": %s\n", p.err);
            free(to_free);  // Shall be freed even when parsing is unsuccessful
        } else {
            exit_status = process_sequenced_commands(&p.s_head, to_free);
            // DO NOT USE `p.s_head` here: it was consumed and destroyed by
            // `process_sequenced_commands`.
            // Same for `to_free`.
        }
    }

    if (WIFEXITED(exit_status))
        return WEXITSTATUS(exit_status);
    else if (WIFSIGNALED(exit_status))
        return 128 + WTERMSIG(exit_status);
    else
        return EXIT_FAILURE;
}
