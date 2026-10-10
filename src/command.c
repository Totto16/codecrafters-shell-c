

#include "./command.h"
#include "./executable.h"

CommandType determine_command_type(tstr_view command) {
	for(size_t i = 0; i < BUILTIN_FUNCTION_COUNT; ++i) {

		BuiltinCommand builtin_command = g_builtin_functions[i];

		if(tstr_view_eq_view(command, tstr_static_as_view(builtin_command.name))) {

			return (CommandType){ .type = CommandTypeTypeBuiltin,
				                  .data = { .builtin = builtin_command } };
		}
	}

	tstr executable_path = get_executable_path(command);

	if(!tstr_is_null(&executable_path)) {
		return (CommandType){ .type = CommandTypeTypeExecutable,
			                  .data = { .executable = { .path = executable_path } } };
	}

	return (CommandType){ .type = CommandTypeTypeNotFound, .data = {} };
}

void free_executable_data(ExecutableData* data) {
	tstr_free(&(data->path));
}
