

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

#define BUILTIN_FUNCTION_COUNT 4

extern BuiltinCommand g_builtin_functions[];

// special function pointer
