
#pragma once

#include <histedit.h>
#include <tstr/tstr.h>

#include "./shell.h"
#include "./utils.h"

typedef struct {
  tstr_view data;
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
    ShellInputOkResult ok;
    tstr error;
  } data;
} ShellInputResult;

typedef struct {
  EditLine *el;
} ShellInputState;

NODISCARD ShellInputResult shell_get_input(ShellInputState *state);
