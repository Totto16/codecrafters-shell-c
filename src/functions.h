#pragma once

#include "./shell.h"

#include <tstr/tstr.h>
#include <tvec/tvec.h>

// TODO: use TRTTI
typedef void *shell_dynamic_args;

typedef void *shell_context;

typedef tstr (*shell_fn_get_current_working_directory)(shell_context ctx);

typedef bool (*shell_fn_set_current_working_directory)(
    shell_context ctx, const tstr *normalized_cwd);

typedef struct {
  bool has_code;
  ExitCode code;
} ExitData;

typedef void (*shell_fn_exit)(shell_context ctx, const ExitData *data);

typedef struct {
  shell_fn_get_current_working_directory get_current_working_directory;
  shell_fn_set_current_working_directory set_current_working_directory;
  shell_fn_exit exit;
} ShellVirtualFunctions;

typedef struct {
  ShellVirtualFunctions function;
  shell_context context;
} ShellHandle;

typedef ExitCode (*shell_invoke)(shell_dynamic_args args,
                                 const ShellHandle shell);

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

typedef tstr_static (*shell_get_help_text)(void);
