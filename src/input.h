
#pragma once

#include <tstr/tstr.h>

#include "./shell.h"
#include "./utils.h"

typedef struct {
  char *data;
} ShellInputOkResult;

void free_shell_input_ok_result(ShellInputOkResult ok);

typedef enum : uint8_t {
  ShellInputResultTypeOk = 0,
  ShellInputResultTypeEOF,
} ShellInputResultType;

typedef struct {
  ShellInputResultType type;
  union {
    ShellInputOkResult ok;
  } data;
} ShellInputResult;

NODISCARD ShellInputResult shell_get_input(const char *const prompt);
