

#pragma once

#include "./builtins.h"
#include "./utils.h"

#include <tstr/tstr.h>

typedef enum : uint8_t {
	CommandTypeTypeBuiltin = 0,
	CommandTypeTypeExecutable,
	CommandTypeTypeNotFound,
} CommandTypeType;

typedef struct {
	tstr path;
} ExecutableData;

typedef struct {
	CommandTypeType type;
	union {
		ExecutableData executable;
		BuiltinCommand builtin;
	} data;
} CommandType;

NODISCARD CommandType determine_command_type(tstr_view command);

void free_executable_data(ExecutableData* data);
