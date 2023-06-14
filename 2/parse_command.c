#include <stdbool.h>
#include <stdlib.h>
#include <assert.h>

struct piped_commands {
    // `argv` itself is owned by this struct, thus shall be freed before freeing
    // the struct. The _elements_ of `argv`, in turn, are _not_ owned by the struct:
    // they point to (parts of the same) string, allocated before the struct was
    // created.
    char **argv;

    // For internal use only
    int _argc;

    // Command to pipe this to. Owned by the struct. `NULL` if shouldn't pipe.
    struct piped_commands *next;

    // Path to file to redirect to. Owned by the struct. `NULL` if shouldn't redirect.
    char *outfile;

    // Append to `outfile`?
    bool append;
};

struct sequenced_commands {
    struct piped_commands *p_head;
    //enum sequencing_type type;
    struct sequenced_commands *next;
};


static struct piped_commands *new_pc();

size_t advance_whitespace(const char *s);

struct sequenced_commands parse_command_line(char *cmd) {
    struct sequenced_commands s_head = {.next = NULL};
    goto all_good;

out:
    // TODO: free everything
    free(s_head.p_head);
    return s_head;

all_good:

    s_head.p_head = new_pc();
    if (!s_head.p_head) {
        goto out;
    }

    char color[strlen(cmd) + 1];
    if (!escape_and_color(cmd, color)) {
        goto out;
    }

    // We are about to make three passes:
    // 1. During the first pass, we will allocate the proper tree of `sequenced_commands`
    //    and `piped_commands` and count the number of command-line arguments each command
    //    will have (but not assign them yet).
    // 2. During the second pass, we will allocate and assign the command-line arguments
    //    for the commands. The arguments pointers will point inside the given `cmd`.
    // 3. During the third pass, we will ensure all the specified command-line arguments are
    //    NULL-terminated (i.e. set some chars of `cmd` to '\0').
    //
    // The first and second steps are not combined together to simplify the implementation by
    // avoiding reallocations: in the first pass I allocate the tree and count all the arguments,
    // then assign them all in the second step.
    //
    // The second and third steps are not combined because in some cases (e.g. "echo 123>f")
    // null-terminating arguments (e.g. "123") will break the command's semantics. It is quite
    // tricky to handle if the command is being read in the same run.

    int total_argc = 0;

    int pos = 0;
    struct sequenced_commands *s_cur = &s_head;
    struct piped_commands *p_cur = s_cur->p_head;
    while (1) {  // First run: build the tree
        pos += advance_whitespace(cmd + pos);

        int read = next_token(cmd + pos);
        if (read < 0) {
            // Unclosed quotation mark
            goto out;
        } else if (read == 0) {
            break;
        }

        if (!strncmp(">", cmd+pos, 1) || !strncmp(">>", cmd+pos, 2)) {
            p_cur->append = (bool)(read - 1);
            // TODO: handle next token
        } else if (!strncmp("|", cmd+pos, 1)) {
            p_cur->next = new_pc();
            if (!p_cur->next) {
                goto out;
            }
            p_cur = p_cur->next;
        } else if (is_word_separator(cmd[pos])) {
            // Comprised of command-special characters (due to the tokenizer properties,
            // it's sufficient to check just the first char) but not a valid operator
            goto out;
        } else {
            // Normal-character argument
            p_cur->_argc++;
        }

        pos += read;
    }

    // TODO: check for dangling pipe

    pos = 0;
    s_cur = &s_head;
    p_cur = s_cur->p_head;
    int cur_arg = 0;
    p_cur->argv = (char **)malloc((p_cur->_argc + 1) * sizeof (char *));
    while (1) {  // Second run: allocate and fill in `argv`s
        pos += advance_whitespace(cmd + pos);

        int read = next_token(cmd + pos);
        // read < 0 is impossible - checked in the previous loop
        assert(read >= 0);
        if (read == 0)
            break;

        if (!strncmp(">", cmd+pos, 1) || !strncmp(">>", cmd+pos, 2)) {
            // Skip the file name
            read += advance_whitespace(cmd + pos + read);
            read += next_token(cmd + pos + read);
        } else if (!strncmp("|", cmd+pos, 1)) {
            // argv shall be NULL-terminated
            p_cur->argv[cur_arg] = NULL;
            p_cur = p_cur->next;
            cur_arg = 0;
            p_cur->argv = (char **)malloc((p_cur->_argc + 1) * sizeof (char *));
            if (!p_cur->argv) {
                // ENOMEM
                goto out;
            }
        } else {
            p_cur->argv[cur_arg++] = cmd + pos;
        }

        pos += read;
    }
    p_cur->argv[cur_arg] = NULL;

    int terminated_times = 1;

    pos = 0;
    while (1) {  // Third run: null-terminate args
        pos += advance_whitespace(cmd + pos);

        int read = next_token(cmd + pos);
        assert(read >= 0);
        if (read == 0)
            break;
        int old = pos;
        pos += read;
        // If the just read token is a usual argument, terminate its string.
        if (!is_word_separator(cmd[old])) {
            cmd[pos++] = '\0';
            ++terminated_times;
        }
    }

    // Finally, restore the escaped characters from colors. There is no problem to do it
    // even so lately because:
    // 1) The first two runs had no effect on `cmd` at all
    // 2) In the last run, we only set some characters to '\0', and we know for sure those
    //    were either whitespaces or (not escaped) command-special characters, which did
    //    not affect the length. Thus, the escaped characters will be restored on their
    //    proper places.
    //
    // Note that `uncolor_unquote` _will_ shift `cmd`. That's why it returns the shift
    // so that we can align it with `color`. We won't need it here, though :)

    int str_shift = 0;
    for (int i = 0; i < terminated_times; ++i) {
        size_t nshift = strlen(cmd + str_shift) + 1;
        (void)uncolor_unquote(cmd + str_shift, color + str_shift);
        str_shift += nshift;
    }

    return s_head;
}

inline static struct piped_commands *new_pc() {
    return (struct piped_commands *)calloc(1, sizeof (struct piped_commands));
}

size_t advance_whitespace(const char *s) {
    size_t res = 0;
    for (; isspace(s[res]); ++res);
    return res;
}
