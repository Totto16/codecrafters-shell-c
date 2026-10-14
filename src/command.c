

#include "./command.h"
#include "./executable.h"

ShellResolvedCommand shell_resolve_command(tstr_view command, const ShellHandle shell) {
	for(size_t i = 0; i < BUILTIN_FUNCTION_COUNT; ++i) {

		BuiltinCommand builtin_command = g_builtin_functions[i];

		if(tstr_view_eq_view(command, tstr_static_as_view(builtin_command.name))) {

			return (ShellResolvedCommand){ .type = CommandTypeBuiltin,
				                           .data = { .builtin = builtin_command } };
		}
	}

	ShellExecutableResult executable_result = get_executable_path(command, shell);

	if(executable_result.is_ok) {
		return (ShellResolvedCommand){ .type = CommandTypeExecutable,
			                           .data = { .executable = {
			                                         .path = executable_result.data.ok,
			                                     } } };
	}

	return (ShellResolvedCommand){ .type = CommandTypeNotFound,
		                           .data = { .not_found = {
		                                         .reason = executable_result.data.error.type,
		                                     } } };
}

void free_executable_data(ShellExecutableData* data) {
	tstr_free(&(data->path));
}
