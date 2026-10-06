#pragma once

#include <tstr/tstr.h>

#include "./functions.h"
#include "./shell.h"

tstr get_executable_path(tstr_view name);

ExitCode invoke_executable(const tstr *path, const tstr *invoke_name,
                           const ShellArguments *arguments);
