
#pragma once

#include <tstr/tstr.h>

#include "./shell.h"

typedef struct {
  char *raw_data;
  tstr_view data;
  bool end;
} ShellInputOkResult;

void free_shell_input_ok_result(ShellInputOkResult ok);

typedef enum : uint8_t {
  ShellInputResultTypeOk = 0,
  ShellInputResultTypeError,
  ShellInputResultTypeEOF,
} ShellInputResultType;

typedef struct {
  ShellInputResultType type;
  union {
    ExitCode err;
    ShellInputOkResult ok;
  } data;
} ShellInputResult;

ShellInputResult shell_get_input(void);
