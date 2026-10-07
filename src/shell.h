

#pragma once

#include <stddef.h>
#include <stdint.h>

#include "./utils.h"

typedef enum : uint8_t {
  ExitCodeSuccess = 0,
  ExitCodeGenericError = 1,
  //
  ExitCodeOOM = 13,
  ExitCodeIOError = 14,
  //
  ExitCodeImplementationError = 42,
  //
  ExitCodeCommandNotFound = 127
} ExitCode;

NODISCARD ExitCode shell_execute_repl(size_t argc, char *const *const argv);
