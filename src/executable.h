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

typedef enum : uint8_t {
	ShellExecuteSyncCaptureResultTypeError = 0,
	ShellExecuteSyncCaptureResultTypeOk,
	ShellExecuteSyncCaptureResultTypeCrash
} ShellExecuteSyncCaptureResultType;

typedef struct {
	bool normal_exit;
	union {
		ShellSignal signal;
		ExitCode exit;
	} data;
} ShellExecuteSyncCaptureResultCrash;

typedef struct {
	tstr data;
} ShellExecuteSyncCaptureResultOk;

typedef struct {
	ShellExecuteSyncCaptureResultType type;
	union {
		tstr error;
		ShellExecuteSyncCaptureResultOk ok;
		ShellExecuteSyncCaptureResultCrash crash;
	} data;
} ShellExecuteSyncCaptureResult;

typedef struct {
	tstr name;
	tstr value;
} ShellEnv;

TVEC_DEFINE_VEC_TYPE(ShellEnv)

typedef TVEC_TYPENAME(ShellEnv) ShellEnvs;

void free_shell_env(ShellEnv* arg);

void free_shell_envs(ShellEnvs* args);

NODISCARD ShellExecuteSyncCaptureResult
invoke_executable_synchronous_capture(const tstr* path, const tstr* invoke_name,
                                      const ShellArguments* arguments, const ShellEnvs* envs);
