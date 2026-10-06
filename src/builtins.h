

#pragma once

#include "./functions.h"

typedef struct {
  shell_parse_arguments parse;
  shell_free_arguments free;
} BuiltinValidator;

typedef struct {
  tstr_static name;
  BuiltinValidator validator;
  shell_invoke invoke;
  shell_get_help_text help;
} BuiltinCommand;

#define BUILTIN_FUNCTION_COUNT 3

extern BuiltinCommand builtin_functions[];

// special function pointer

typedef struct {
  bool has_code;
  ExitCode code;
} ExitData;

extern ExitCode exit_invoke(shell_dynamic_args args);
