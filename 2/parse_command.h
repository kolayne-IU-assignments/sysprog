#pragma once

#include <stdbool.h>

/**
 * A linked list of commands piped into each other.
 * For the last command in the pipe sequence, `.next` is `NULL`.
 */
struct piped_commands {
    // `argv` itself is owned by this struct, thus shall be freed before freeing
    // the struct. The _elements_ of `argv`, in turn, are _not_ owned by the struct:
    // they point to (parts of the same) string allocated independently of this
    // struct.
    char **argv;

    // For internal use only
    int _argc;

    // Command to pipe this to. Owned by the struct. `NULL` if shouldn't pipe.
    struct piped_commands *next;

    // Path to file to redirect to. Not owned by the struct. `NULL` if shouldn't redirect.
    char *outfile;

    // Append to `outfile`?
    bool append;
};

/// Allocate and default-initialize a `struct piped_commands`
static struct piped_commands *new_pc();

/**
 * A linked list of groups of piped commands combined in a conditional sequence.
 * For the last command group, `.next` is `NULL`.
 */
struct sequenced_commands {
    struct piped_commands *p_head;
    //enum sequencing_type type;
    struct sequenced_commands *next;
};

/**
 * Cleanup all the memory owned by `pc` and its children.
 * Invalidates the object but does not free `pc` itself.
 */
void destroy_piped_commands(struct piped_commands *pc);


/**
 * Cleanup all the memory owned by `sc` and its children.
 * Invalidates the object but does not free `sc` itself.
 */
void destroy_sequenced_commands(struct sequenced_commands *sc);


struct parse_result {
    const char *err;
    struct sequenced_commands s_head;
};

/**
 * Parses the given command line into a `struct sequenced_commands`. Returns
 * a `struct parse_result`. On success, `.s_head` is the parsed command and
 * `.err` is `NULL`. The value of `.s_head` shall be destroyed with
 * `destroy_sequenced_commands` to free the resources.
 * On error, `.err` is set to the error message, `.s_head` is an invalid object
 * that shall not be used nor destroyed.
 */
struct parse_result parse_command_line(char *cmd);
