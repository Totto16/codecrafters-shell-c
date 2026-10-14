
#pragma once

#include "./utils.h"

#include <tstr/tstr.h>

NODISCARD bool path_is_absolute(tstr_view path);

NODISCARD tstr_view path_normalize_dir_ending(tstr_view path);

NODISCARD bool path_exists_and_is_dir(tstr_view path);

typedef enum : uint8_t {
	ShellExecutableErrorTypeGeneric = 0,
	ShellExecutableErrorTypeNotFound,
	ShellExecutableErrorTypeNoPermission
} ShellExecutableErrorType;

typedef struct {
	ShellExecutableErrorType type;
} ShellExecutableError;

typedef struct {
	bool is_ok;
	union {
		tstr ok;
		ShellExecutableError error;
	} data;
} ShellExecutableResult;

NODISCARD ShellExecutableResult path_dir_get_executable_from_parts(tstr_view dir, tstr_view name);

NODISCARD ShellExecutableResult path_dir_get_executable(tstr_view path);

NODISCARD tstr path_get_cwd(void);

NODISCARD tstr path_normalize(tstr_view path);

NODISCARD bool path_is_normalized(tstr_view path);

NODISCARD tstr path_resolve_to_absolute(tstr_view cwd, tstr_view path);
