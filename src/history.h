

#pragma once

#include "./functions.h"
#include "./utils.h"

typedef tstr (*read_history_to_file_cb)(shell_virtual_context ctx, tstr_view entry);

NODISCARD tstr read_history_to_file(const tstr* file, read_history_to_file_cb cb,
                                    shell_virtual_context ctx);

NODISCARD tstr write_history_to_file(const ShellHistory* history, const tstr* file);

NODISCARD tstr append_history_to_file(const ShellHistory* history, const tstr* file,
                                      size_t start_idx);
