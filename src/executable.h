#pragma once

#include <tstr/tstr.h>

#include "./functions.h"
#include "./parse.h"
#include "./shell.h"
#include "./utils.h"

NODISCARD tstr get_executable_path(tstr_view name);

NODISCARD ExitCode invoke_executable(const tstr *path, const tstr *invoke_name,
                                     const ShellArguments *arguments,
                                     const ShellRedirectSettings *settings);
