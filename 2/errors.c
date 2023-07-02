const char err_oom[] = "Internal error: out of memory";
const char err_trailing_backslash[] = "Parse error: backslash at the end of input";
const char err_unclosed_quot[] = "Parse error: unclosed quotation mark at the end of input";
const char err_trailing_redir[] = "Parse error: redirect at the end of the command";
const char err_invalid_filename[] = "Parse error: redirection filename contains special characters";
const char err_invalid_operator[] = "Parse error: invalid operator";
const char err_argless_command[] = "Parse error: encountered a command with no arguments (a pipe at the end of the command?)";
const char err_input_is_over[] = "";  // Never printed to user
