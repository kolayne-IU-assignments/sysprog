#include <stdbool.h>
#include <stdlib.h>
#include <assert.h>
#include <string.h>
#include <ctype.h>

#include "tokenizer.h"
#include "errors.h"
#include "parse_command.h"

void destroy_piped_commands(struct piped_commands *pc) {
    free(pc->argv);
    if (pc->next)
        destroy_piped_commands(pc->next);
    free(pc->next);
}

void destroy_sequenced_commands(struct sequenced_commands *sc) {
    if (sc->next)
        destroy_sequenced_commands(sc->next);
    if (sc->p_head)  // Should always be true
        destroy_piped_commands(sc->p_head);
    free(sc->p_head);
    free(sc->next);
}


size_t advance_whitespace(const char *s);

bool is_operator(const char *op, const char *s) {
    int i = 0;
    for (; op[i] && s[i]; ++i) {
        if (op[i] != s[i])
            return false;
    }
    if (!op[i] && !is_cm_special(s[i]))
        return true;
    return false;
}


/// Allocate and default-initialize a `struct piped_commands`
static struct piped_commands *new_pc();

struct parse_result parse_command_line(char *cmd) {
    struct parse_result res = {.err = NULL, .s_head = {.next = NULL}};
    goto normal;

err_out:
    destroy_sequenced_commands(&res.s_head);
    return res;

normal:

    res.s_head.p_head = new_pc();
    if (!res.s_head.p_head) {
        res.err = err_oom;
        goto err_out;
    }

    char color[strlen(cmd) + 1];
    if (!escape_and_color(cmd, color)) {
        res.err = err_trailing_backslash;
        goto err_out;
    }

    // We are about to make three passes:
    // 1. During the first pass we will allocate the proper tree of `sequenced_commands`
    //    and `piped_commands` and count the number of command-line arguments each command
    //    will have (but not assign them yet).
    // 2. During the second pass we will allocate and assign the command-line arguments
    //    for the commands. The arguments pointers will point inside the given `cmd`.
    // 3. During the third pass we will ensure all the specified command-line arguments are
    //    NULL-terminated (by setting some chars of `cmd` to '\0').
    //
    // The first and second steps are not combined together to simplify the implementation by
    // avoiding reallocations: in the first pass I allocate the tree and count all the arguments,
    // then assign them all in the second step.
    //
    // The second and third steps are not combined because in some cases (e.g. "echo 123>f")
    // blind-ly null-terminating arguments (e.g. "123") in-place will break the
    // command's semantics. It is quite tricky to handle if the command is being read in the
    // same run.

    int pos = 0;
    struct sequenced_commands *s_cur = &res.s_head;
    struct piped_commands *p_cur = s_cur->p_head;
    while (1) {  // First pass: build the tree
        pos += advance_whitespace(cmd + pos);

        int read = next_token(cmd + pos);
        if (read < 0) {
            // Unclosed quotation mark
            res.err = err_unclosed_quot;
            goto err_out;
        } else if (read == 0) {
            break;
        }

        if (is_operator(">", cmd+pos) || is_operator(">>", cmd+pos)) {
            p_cur->append = (bool)(read - 1);
            read += advance_whitespace(cmd + pos + read);
            int next_read = next_token(cmd + pos + read);
            if (!next_read) {
                res.err = err_trailing_redir;
                goto err_out;
            }
            if (is_cm_special(cmd[pos + read])) {
                res.err = err_invalid_filename;
                goto err_out;
            }
            p_cur->outfile = cmd + pos + read;
            read += next_read;
        } else if (is_operator("|", cmd+pos)) {
            p_cur->next = new_pc();
            if (!p_cur->next) {
                res.err = err_oom;
                goto err_out;
            }
            p_cur = p_cur->next;
        } else if (is_cm_special(cmd[pos])) {
            // Comprised of command-special characters but is not a valid operator.
            // (note: it's sufficient to check just the first char due to the tokenizer
            // properties)
            res.err = err_invalid_operator;
            goto err_out;
        } else {
            // Normal-character argument
            p_cur->_argc++;
        }

        pos += read;
    }

    if (!p_cur->_argc) {
        res.err = err_argless_command;
        goto err_out;
    }

    pos = 0;
    s_cur = &res.s_head;
    p_cur = s_cur->p_head;
    int cur_arg = 0;
    p_cur->argv = (char **)malloc((p_cur->_argc + 1) * sizeof (char *));
    if (!p_cur->argv) {
        res.err = err_oom;
        goto err_out;
    }
    while (1) {  // Second pass: allocate and fill in `argv`s
        pos += advance_whitespace(cmd + pos);

        int read = next_token(cmd + pos);
        // read < 0 is impossible - checked in the previous loop
        assert(read >= 0);
        if (read == 0)
            break;

        if (is_operator(">", cmd+pos) || is_operator(">>", cmd+pos)) {
            // Skip the file name
            read += advance_whitespace(cmd + pos + read);
            read += next_token(cmd + pos + read);
        } else if (is_operator("|", cmd+pos)) {
            // argv shall be NULL-terminated
            p_cur->argv[cur_arg] = NULL;
            p_cur = p_cur->next;
            cur_arg = 0;
            p_cur->argv = (char **)malloc((p_cur->_argc + 1) * sizeof (char *));
            if (!p_cur->argv) {
                res.err = err_oom;
                goto err_out;
            }
        } else {
            p_cur->argv[cur_arg++] = cmd + pos;
        }

        pos += read;
    }
    // Last command's argv shall be NULL-terminated too!
    p_cur->argv[cur_arg] = NULL;

    int terminated_times = 0;
    pos = 0;
    bool done = false;
    while (!done) {  // Third pass: null-terminate args
        pos += advance_whitespace(cmd + pos);

        int read = next_token(cmd + pos);
        assert(read >= 0);
        if (read == 0)
            break;
        int old = pos;
        pos += read;
        // If the just read token is a usual argument or a file name, terminate its string.
        if (!is_cm_special(cmd[old])) {
            done = cmd[pos] == '\0';
            cmd[pos++] = '\0';
            ++terminated_times;
        }
    }

    // Finally, restore the escaped characters from colors. There is no problem to do it
    // even this late because:
    // - The first two runs had no effect on `cmd` at all
    // - In the last run, we only set some characters to '\0' and we know for sure those
    //   were either whitespaces or (not escaped) command-special characters. The length
    //   did not change. Thus, the escaped characters will be restored on their proper
    //   places.
    //
    // Note that `uncolor_unquote` _will_ shift `cmd`. That's why it returns the shift
    // so that we can align it with `color`. We won't need it here, though :)

    int str_shift = 0;
    for (int i = 0; i < terminated_times; ++i) {
        size_t nshift = strlen(cmd + str_shift) + 1;
        (void)uncolor_unquote(cmd + str_shift, color + str_shift);
        str_shift += nshift;
    }

    return res;
}

inline static struct piped_commands *new_pc() {
    return (struct piped_commands *)calloc(1, sizeof (struct piped_commands));
}

size_t advance_whitespace(const char *s) {
    size_t res = 0;
    for (; isspace(s[res]); ++res);
    return res;
}
