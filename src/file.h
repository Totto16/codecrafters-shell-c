
#pragma once

#include "./utils.h"

#include <tstr/tstr.h>

NODISCARD bool path_is_absolute(tstr_view path);

NODISCARD tstr_view path_normalize_dir_ending(tstr_view path);

NODISCARD bool path_exists_and_is_dir(tstr_view path);

NODISCARD tstr path_dir_get_executable(tstr_view dir, tstr_view name);

NODISCARD tstr path_get_cwd(void);

NODISCARD tstr path_normalize(tstr_view path);

NODISCARD bool path_is_normalized(tstr_view path);

NODISCARD tstr path_resolve_to_absolute(tstr_view cwd, tstr_view path);

NODISCARD int shell_redirect_open_file(const tstr *const path);

void shell_redirect_close_file(int fd);
