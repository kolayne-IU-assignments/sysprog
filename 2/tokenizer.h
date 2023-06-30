#include <stdbool.h>

/*
 * In the commands parsing process there are two kinds of characters:
 * _usual_ (have no special meaning whatsoever) and _special_ (have some
 * special syntactical meaning either at the low parsing level (the ones that
 * affect the parsing itself) or at the high parsing level (the ones that
 * only affect the semantics of the shell command but not the parsing process).
 * Low-level special symbols are also referred to as _parser-special_,
 * high-level special symbols are referred to as _command-special_.
 *
 * Parser-special symbols are: backalsh (\) and quotation mark (").
 * Command-special symbols are: vertical slash (|) and
 * the greater symbol (>).
 * Usual symbols are all the non-special characters, except for '\0'.
 * The null character is treated as an end of string, unless stated otherwise.
 *
 * The parsing process is implemented using the character coloring, which is
 * rather memory-inefficient (requires O(N) auxilary memory) but quite
 * convenient to implemented.
 */

/**
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
bool escape_and_color(char *original, char *color);


#define WHITESPACE " \f\n\r\t\v"  // According to isspace(3)
#define COMMAND_SPECIAL ">|"  // TODO: & and ; are coming later

/**
 * Returns `true` if the given character is either a whitespace
 * (as in isspace(3)), or a string terminator `\0`, or a command-special
 * character, which indicates that the current token is over.
 */
bool is_word_separator(char c);

/**
 * Returns `true` if the given character is command-special.
 */
bool is_cm_special(char c);


/**
 * Finds the next token and returns its length.
 * Input string must not begin with whitespace.
 *
 * If there is an unclosed quotation, `-1` is returned. If there is nothing
 * left to read (or `*inp` is a whitespace symbol), 0 is returned.
 */
int next_token(const char *inp);


/**
 * Removes the quotation marks from the original null-terminated string, shifting
 * its other characters to the left, and restores the escaped characters from
 * `color` (if there was an escaped quotation mark, it is restored as if a usual
 * character). Returns the number of quotation marks found (i.e. the left shift
 * of the end of the string).
 */
int uncolor_unquote(char *s, const char *color);
