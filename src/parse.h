
#pragma once

#include "./builtins.h"
#include "./utils.h"

typedef struct {
	tstr name;
} ShellCommandNotFound;

typedef enum : uint8_t { ShellCommandTypeBuiltin = 0, ShellCommandTypeExecutable } ShellCommandType;

typedef struct {
	tstr path;
	tstr invoke_name;
} ShellCommandExecutable;

typedef struct {
	ShellCommandType type;
	union {
		ShellCommandBuiltin builtin;
		ShellCommandExecutable executable;
	} data;
	ShellArgument arg0;
	ShellArguments args;
	ShellRedirectSettings settings;
} ShellCommand;

TVEC_DEFINE_VEC_TYPE(ShellCommand)

typedef TVEC_TYPENAME(ShellCommand) ShellCommands;

typedef struct {
	ShellCommands commands;
} ShellParseDataPipeline;

typedef enum : uint8_t {
	ShellParseDataResultTypeError = 0,
	ShellParseDataResultTypePipeline,
	ShellParseDataResultTypeCommandsNotFound,
	ShellParseDataResultTypeEmpty,
} ShellParseDataResultType;

TVEC_DEFINE_VEC_TYPE(ShellCommandNotFound)

typedef TVEC_TYPENAME(ShellCommandNotFound) ShellCommandNotFoundArray;

typedef struct {
	ShellCommandNotFoundArray array;
} ShellCommandsNotFound;

typedef struct {
	ShellParseDataResultType type;
	union {
		ShellParseDataPipeline pipeline;
		ShellCommandsNotFound not_found;
		tstr error;
	} data;
} ShellParseDataResult;

NODISCARD ShellParseDataResult shell_parse_data(tstr_view input, const ShellHandle shell);

void free_shell_pipeline(ShellParseDataPipeline* pipeline);

void free_shell_commands_not_found(ShellCommandsNotFound* not_found);

void free_shell_argument(ShellArgument* arg);

void free_shell_arguments(ShellArguments* args);
