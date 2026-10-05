
#pragma once

#include <tstr/tstr.h>

#include "./shell.h"

typedef struct {
  char *raw_data;
  tstr_view data;
} ShellInputOkResult;

void free_shell_input_ok_result(ShellInputOkResult ok);

typedef struct {
  bool is_ok;
  union {
    ExitCode err;
    ShellInputOkResult ok;
  } data;
} ShellInputResult;

ShellInputResult shell_get_input(void);
