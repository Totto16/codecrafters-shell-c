

#pragma once

#include <stdint.h>

typedef enum : uint8_t {
  ExitCodeSuccess = 0,
  ExitCodeGenericError = 1,
  //
  ExitCodeOOM = 13,
  //
  ExitCodeCommandNotFound = 127
} ExitCode;

ExitCode shell_execute_repl(void);
