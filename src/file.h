
#pragma once

#include <tstr/tstr.h>

bool path_exists_and_is_dir(tstr_view dir);

tstr path_dir_get_executable(tstr_view dir, tstr_view name);
