#pragma once

#include "./shell.h"

#include <trtti/trtti.h>
#include <tstr/tstr.h>
#include <tvec/tvec.h>

typedef RTTIAnnotatedPtr shell_dynamic_args;

typedef RTTIAnnotatedValue shell_virtual_context;

typedef RTTIAnnotatedPtr shell_io_context;

typedef tstr_view (*shell_fn_get_current_working_directory)(shell_virtual_context ctx);

typedef bool (*shell_fn_set_current_working_directory)(shell_virtual_context ctx,
                                                       const tstr* normalized_cwd);

typedef struct {
	bool has_code;
	ExitCode code;
} ExitData;

TRTTI_DECLARE_TYPE_AS_SUPPORTED(ExitData)

typedef void (*shell_fn_exit)(shell_virtual_context ctx, const ExitData* data);

typedef bool (*shell_fn_add_to_history)(shell_virtual_context ctx, tstr_view command);

TVEC_DEFINE_VEC_TYPE(tstr)

typedef TVEC_TYPENAME(tstr) TstrArray;

typedef struct {
	TstrArray array;
} ShellHistory;

typedef const ShellHistory* (*shell_fn_get_history)(shell_virtual_context ctx);

typedef enum : uint8_t {
	HistoryProcessTypeRead = 0,
	HistoryProcessTypeWrite,
	HistoryProcessTypeAppend
} HistoryProcessType;

typedef struct {
	HistoryProcessType type;
	tstr file;
} HistoryProcessData;

typedef tstr (*shell_fn_process_history_file)(shell_virtual_context ctx,
                                              const HistoryProcessData* data);

typedef struct {
	tstr script;
} ShellCompletion;

typedef const ShellCompletion* (*shell_fn_get_completion)(shell_virtual_context ctx,
                                                          const tstr* name);

typedef bool (*shell_fn_set_completion)(shell_virtual_context ctx, tstr name,
                                        ShellCompletion completion);

typedef struct {
	shell_fn_get_current_working_directory get_current_working_directory;
	shell_fn_set_current_working_directory set_current_working_directory;
	shell_fn_exit exit;
	shell_fn_add_to_history add_to_history;
	shell_fn_get_history get_history;
	shell_fn_process_history_file process_history_file;
	shell_fn_get_completion get_completion;
	shell_fn_set_completion set_completion;
} ShellVirtualFunctions;

typedef struct {
	ShellVirtualFunctions function;
	shell_virtual_context context;
} ShellHandle;

typedef __attribute__((format(printf, 3, 4))) int (*io_fprintf)(shell_io_context ctx,
                                                                FILE* restrict stream,
                                                                const char* restrict format, ...);

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
