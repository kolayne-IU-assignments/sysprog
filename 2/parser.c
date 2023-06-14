#include <stdbool.h>

/**
 * A memory-inefficient but very implementation-convenient way to handle
 * backslash-escaped characters.
 *
 * Accepts two parameters: the NULL-terminated input string `original` and
 * a character array `color`, which must have the capacity to store `original`
 * (i.e. of size `strlen(original) + 1`), may not be initialized.
 *
 * The original string is modified such that the _escaping_ backslashes are
 * removed (the rest of the string is shifted) and the _escaped_ characters
 * are replaced with some placeholder, while the actual character is stored
 * in the corresponding element of `color`. For the usual (non-placeholder)
 * characters the value of `color` is `\0`.
 *
 * Such an approach allows to easily tokenize the original string (all the
 * escaped special characters are removed, if any special characters are left,
 * they really should be treated as special); after the tokenization it is
 * easy to restore the escaped symbols to get the desired literal string.
 *
 * If the original string ends with a backslash, returns `false` (the
 * `original` and `color` are still modified in this case). Otherwise,
 * the parse is successful and `true` is returned.
 */
bool escape_and_color(char *const original, char *const color) {
    // Invariant: `write <= read`. In case of backslash, the string
    // is "shifted" back (and the corresponding char is colored).
    int read = 0, write = 0;
    for (; original[read]; ++read, ++write) {
        if (original[read] == '\\') {
            if (!original[read + 1])
                return false;

            ++read;
            // The actual protected symbol is stored in `color[write]` and will
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

#define WHITESPACE " \f\n\r\t\v"  // According to isspace(3)
#define SPECIAL " >|"  // TODO: & and ; are coming later

/**
 * Returns `true` if the given character s either a whitespace
 * (as in isspace(3)), or a string terminator `\0`, or a special character,
 * which indicates that the current token is over.
 */
inline static bool is_word_separator(char c) {
    return (bool)strchr(WHITESPACE SPECIAL, c);
}

/**
 * Finds the next token and returns its length.
 * Input string must not begin with whitespace.
 *
 * If there is an unclosed quotation, `-1` is returned. If there is nothing
 * left to read (or `*inp` is a whitespace symbol), 0 is returned.
 */
int next_token(const char *const inp) {
    int pos = 0;

    do {
        // Try to read a non-special token

        int read = 0;
        // Read symbols that are neither a whitespace nor special (including
        // quotation).
        int res = sscanf(inp + pos, "%*[^\"" WHITESPACE SPECIAL "]%n", &read);

        // If the next character is not a quotation, nothing to read further
        pos += read;
        if (inp[pos] != '"')
            break;
        ++pos;

        // If _is_ a quotation, read until the next quotation
        read = 0;
        res = sscanf(inp + pos, "%*[^\"]%n", &read);
        if (res == EOF || inp[pos + read] != '"') {
            return -1;
        }
        pos += read + 1;  // +1 for quotation

        // Now that we have read the stuff to the quotation, it still doesn't
        // mean the argument is over! For example, in the command
        // `cat 123"456"789` there's just one command-line argument to cat,
        // according to bash. So, unless the next character is an argument
        // separator (or the end of string), should continue parsing.
    } while (!is_word_separator(inp[pos]));

    // If we read something, then this token is a literal string, just
    // return it.
    if (pos)
        return pos;

    // But! The current token could be special, try to scan it here.

    // Try to read, modifying `pos` directly. Regardless of `sscanf`'s success,
    // should return the position we moved to (which remains zero if there was
    // nothing to scan).
    (void)sscanf(inp, "%*[" SPECIAL "]%n", &pos);
    return pos;
}
