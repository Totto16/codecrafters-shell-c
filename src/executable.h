#pragma once

#include <tstr/tstr.h>

#include "./functions.h"
#include "./parse.h"
#include "./shell.h"
#include "./utils.h"

typedef struct {
	tstr_view path;
} PathDirectory;

TVEC_DEFINE_VEC_TYPE(PathDirectory)

typedef TVEC_TYPENAME(PathDirectory) PathDirectories;

typedef struct {
	bool is_ok;
	union {
		PathDirectories directories;
		tstr error;
	} data;
} PathDirectoriesResult;

void free_path_directories(PathDirectories* directories);

NODISCARD PathDirectoriesResult get_path_directories(tstr_view path_env);

NODISCARD tstr get_executable_path(tstr_view name);

typedef uint8_t ShellSignal;

typedef enum : uint8_t {
	ShellExecuteSyncResultTypeError = 0,
	ShellExecuteSyncResultTypeExit,
	ShellExecuteSyncResultTypeSignal
} ShellExecuteSyncResultType;

typedef struct {
	ShellExecuteSyncResultType type;
	union {
		tstr error;
		ShellSignal signal;
		ExitCode exit;
	} data;
} ShellExecuteSyncResult;

NODISCARD ShellExecuteSyncResult invoke_executable_synchronous(
    const tstr* path, const tstr* invoke_name, const ShellArguments* arguments,
    const ShellRedirectSettings* settings);

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

NODISCARD ShellExecuteAsyncResult invoke_executable_async(const tstr* path, const tstr* invoke_name,
                                                          const ShellArguments* arguments,
                                                          const ShellRedirectSettings* settings,
                                                          const ShellPipeState* pipe_state);
