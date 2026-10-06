
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
