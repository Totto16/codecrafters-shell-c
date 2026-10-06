
#pragma once

#include <tstr/tstr.h>

bool path_is_absolute(tstr_view path);

tstr_view path_normalize_dir_ending(tstr_view path);

bool path_exists_and_is_dir(tstr_view path);

tstr path_dir_get_executable(tstr_view dir, tstr_view name);

tstr path_get_cwd(void);
