#pragma once

#include <sys/types.h>

#include "parse_command.h"

void process_sequenced_commands(const struct sequenced_commands *sc);
void process_piped_commands(const struct piped_commands *pc, int write_my_pid_fd);
