

#pragma once

#include <stdint.h>

typedef enum : uint8_t {
  ExitCodeSuccess = 0,
} ExitCode;

ExitCode shell_execute_repl(void);
