#pragma once

#include <tstr/tstr.h>

#include "./functions.h"
#include "./parse.h"
#include "./shell.h"
#include "./utils.h"

NODISCARD tstr get_executable_path(tstr_view name);

NODISCARD ExitCode invoke_executable_synchronous(
    const tstr *path, const tstr *invoke_name, const ShellArguments *arguments,
    const ShellRedirectSettings *settings);

typedef struct {
  pid_t child_pid;
} ShellExecuteAsyncHandle;

typedef struct {
  bool is_ok;
  union {
    tstr error;
    ShellExecuteAsyncHandle handle;
  } data;
} ShellExecuteAsyncResult;

NODISCARD ShellExecuteAsyncResult invoke_executable_async(
    const tstr *path, const tstr *invoke_name, const ShellArguments *arguments,
    const ShellRedirectSettings *settings,
    const ShellPipeSettings *pipe_settings);
