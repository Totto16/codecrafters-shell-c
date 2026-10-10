#pragma once

#include "./shell.h"

#include <tstr/tstr.h>
#include <tvec/tvec.h>

// TODO: use TRTTI
typedef void* shell_dynamic_args;

typedef void* shell_virtual_context;

typedef void* shell_io_context;

typedef tstr_view (*shell_fn_get_current_working_directory)(shell_virtual_context ctx);

typedef bool (*shell_fn_set_current_working_directory)(shell_virtual_context ctx,
                                                       const tstr* normalized_cwd);

typedef struct {
	bool has_code;
	ExitCode code;
} ExitData;

typedef void (*shell_fn_exit)(shell_virtual_context ctx, const ExitData* data);

typedef struct {
	shell_fn_get_current_working_directory get_current_working_directory;
	shell_fn_set_current_working_directory set_current_working_directory;
	shell_fn_exit exit;
} ShellVirtualFunctions;

typedef struct {
	ShellVirtualFunctions function;
	shell_virtual_context context;
} ShellHandle;

typedef int (*io_fprintf)(shell_io_context ctx, FILE* restrict stream, const char* restrict format,
                          ...);

typedef struct {
	io_fprintf fprintf;
} ShellVirtualIOFunctions;

typedef struct {
	ShellVirtualIOFunctions function;
	shell_io_context context;
} ShellIO;

typedef ExitCode (*shell_invoke)(shell_dynamic_args args, const ShellHandle shell,
                                 const ShellIO io);

typedef struct {
	tstr_view original;
	tstr parsed;
} ShellArgument;

TVEC_DEFINE_VEC_TYPE(ShellArgument)

typedef TVEC_TYPENAME(ShellArgument) ShellArguments;

typedef struct {
	bool is_ok;
	union {
		tstr error;
		shell_dynamic_args args;
	} data;
} ShellParseArgumentsResult;

typedef ShellParseArgumentsResult (*shell_parse_arguments)(const ShellArguments* args);

typedef void (*shell_free_arguments)(shell_dynamic_args args);

typedef tstr_static (*shell_get_help_text)(void);
