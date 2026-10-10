

#pragma once

#include "./functions.h"

typedef struct {
	shell_parse_arguments parse;
	shell_free_arguments free;
} BuiltinValidator;

typedef struct {
	tstr_static name;
	BuiltinValidator validator;
	shell_invoke invoke;
	shell_get_help_text help;
} BuiltinCommand;

#define BUILTIN_FUNCTION_COUNT 5

extern BuiltinCommand g_builtin_functions[];

typedef struct {
	BuiltinCommand command;
	shell_dynamic_args args;
} ShellCommandBuiltin;

typedef struct {
	tstr location;
	bool append;
} ShellRedirectLocation;

typedef struct {
	ShellRedirectLocation stdout;
	ShellRedirectLocation stderr;
} ShellRedirectSettings;

NODISCARD bool shell_redirect_is_set(const ShellRedirectLocation* location);

NODISCARD int shell_redirect_open_file(const ShellRedirectLocation* location);

void shell_redirect_close_file(int fd);

ExitCode invoke_shell_builtin_in_current_context(const ShellCommandBuiltin* builtin,
                                                 ShellHandle shell,
                                                 const ShellRedirectSettings* settings);

#define PIPE_READ_END 0
#define PIPE_WRITE_END 1

#define PIPE_AMOUNT 2

typedef struct {
	int fds[PIPE_AMOUNT];
} ShellPipeState;

typedef struct {
	bool is_pipe;
	ShellPipeState state;
} ShellMaybePipe;

typedef struct {
	ShellMaybePipe stdin;
	ShellMaybePipe stdout;
} ShellPipeSettings;

ExitCode invoke_shell_builtin_in_subshell(const ShellCommandBuiltin* builtin, ShellHandle shell,
                                          const ShellRedirectSettings* settings,
                                          const ShellPipeSettings* pipe_settings);
