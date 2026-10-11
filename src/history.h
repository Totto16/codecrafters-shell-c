

#pragma once

#include "./functions.h"
#include "./utils.h"

typedef bool (*read_history_to_file_cb)(shell_virtual_context ctx, tstr_view entry);

NODISCARD bool read_history_to_file(const tstr* file, read_history_to_file_cb cb,
                                    shell_virtual_context ctx);

NODISCARD bool write_history_to_file(const ShellHistory* history, const tstr* file, bool append);
