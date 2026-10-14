

#pragma once

#include "./builtins.h"
#include "./executable.h"
#include "./utils.h"

#include <tstr/tstr.h>

typedef enum : uint8_t {
	CommandTypeBuiltin = 0,
	CommandTypeExecutable,
	CommandTypeNotFound,
} CommandType;

typedef struct {
	tstr path;
} ShellExecutableData;

typedef struct {
	ShellExecutableErrorType reason;
} ShellCommandNotFoundData;

typedef struct {
	CommandType type;
	union {
		ShellExecutableData executable;
		BuiltinCommand builtin;
		ShellCommandNotFoundData not_found;
	} data;
} ShellResolvedCommand;

NODISCARD ShellResolvedCommand shell_resolve_command(tstr_view command, ShellHandle shell);

void free_executable_data(ShellExecutableData* data);
