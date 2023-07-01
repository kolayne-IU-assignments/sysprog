#include <stdbool.h>
#include <string.h>
#include <stdio.h>
#include <assert.h>

#include "tokenizer.h"

bool escape_and_color(char *const original, char *const color) {
    // Invariant: `write <= read`. In case of backslash, the string
    // is "shifted" back (and the corresponding char is colored).
    int read = 0, write = 0;
    for (; original[read]; ++read, ++write) {
        if (original[read] == '\\') {
            if (!original[read + 1])
                return false;

            ++read;
            // The actual escaped symbol is stored in `color[write]` and will
            // be interpreted as a raw literal;
            // In the original string the corresponding character is replaced with
            // a placeholder. The value of the placeholder doesn't really matter,
            // except it must not be a character treated specially by our shell.
            color[write] = original[read];
            original[write] = '_';
        } else {
            // If not a backslash, store the character as is and make sure to
            // mark it as uncolored.
            original[write] = original[read];
            color[write] = '\0';
        }
    }
    original[write] = original[read];  // Copy the `\0` too
    return true;
}

bool is_word_separator(char c) {
    return (bool)strchr(WHITESPACE COMMAND_SPECIAL, c);
}

bool is_cm_special(char c) {
    return c && (bool)strchr(COMMAND_SPECIAL, c);
}

int next_token(const char *const inp) {
    int pos = 0;

    char quot_fmt[] = "%*[^\1]%n";
    const size_t quot = 4;
    assert(quot_fmt[quot] == '\1');

    do {
        // Try to read a non-special token

        int read = 0;
        // Read symbols that are neither a whitespace nor command-special
        // nor a quotation mark.
        (void)sscanf(inp + pos, "%*[^\"'" WHITESPACE COMMAND_SPECIAL "]%n",
            &read);

        // If the next character is not a quotation mark, nothing to read further
        pos += read;
        if (inp[pos] != '"' && inp[pos] != '\'')
            break;

        // If _is_ a quotation, read until the next quotation mark
        quot_fmt[quot] = inp[pos];
        ++pos;
        read = 0;
        int res = sscanf(inp + pos, quot_fmt, &read);
        if (res == EOF || inp[pos + read] != quot_fmt[quot]) {
            return -1;
        }
        pos += read + 1;  // +1 for the quotation mark

        // Now that we have read the stuff to the quotation, it still doesn't
        // mean the argument is over! For example, in the command
        // `cat 123"456"789` there's just one command-line argument to cat,
        // according to bash. So, unless the next character is an argument
        // separator (or the end of string), should continue parsing.
    } while (!is_word_separator(inp[pos]));

    // If we read something then this token is a literal string, just
    // return it.
    if (pos)
        return pos;

    // But! The current token could be special, try to scan it here.

    // Try to read, modifying `pos` directly. Regardless of `sscanf`'s success,
    // should return the position we moved to (which remains zero in case there
    // was nothing to scan).
    (void)sscanf(inp, "%*[" COMMAND_SPECIAL "]%n", &pos);
    return pos;
}

int uncolor_unquote(char *const s, const char *const color) {
    int read = 0, write = 0;
    char current_quote = '\0';
    for (; s[read]; ++read, ++write) {
        if (s[read] == current_quote) {
            current_quote = '\0';
            ++read;
        } else if (!current_quote && (s[read] == '"' || s[read] == '\'')) {
            current_quote = s[read++];
        }

        if (!s[read])
            break;

        if (color[read])
            s[write] = color[read];
        else
            s[write] = s[read];
    }
    s[write] = s[read];  // Copy the last '\0' too
    return read - write;
}
