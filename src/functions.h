#pragma once

#include "./shell.h"

#include <tstr/tstr.h>
#include <tvec/tvec.h>

// TODO: use TRTTI
typedef void *shell_dynamic_args;

typedef ExitCode (*shell_invoke)(shell_dynamic_args args);

typedef struct {
  tstr_view original;
  tstr parsed;
} ShellArgument;

TVEC_DEFINE_VEC_TYPE(ShellArgument)

typedef TVEC_TYPENAME(ShellArgument) ShellArguments;

typedef struct {
  bool is_ok;
  union {
    tstr error;
    shell_dynamic_args args;
  } data;
} ShellParseArgumentsResult;

typedef ShellParseArgumentsResult (*shell_parse_arguments)(
    const ShellArguments *args);

typedef void (*shell_free_arguments)(shell_dynamic_args args);

typedef tstr (*shell_get_help_text)(void);
