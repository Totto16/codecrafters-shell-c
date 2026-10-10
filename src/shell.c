#include "./shell.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <pthread.h>
#include <sys/wait.h>
#include <tstr/tstr.h>

#include "./builtins.h"
#include "./executable.h"
#include "./file.h"
#include "./input.h"
#include "./parse.h"
#include "./utils.h"

typedef struct {
	ExitCode exit_code;
} CommandState;

typedef struct {
	bool alive;
	ExitCode result;
	tstr cwd;
} ShellInstanceState;

typedef struct {
	CommandState command_state;
	const bool interactive;
	ShellInputState input_state;
	ShellInstanceState global;
} ShellState;

static tstr shell_get_prompt(const ShellState* const state) {

	// TODO. support setting the prompt dynamically
	UNUSED(state);

	return TSTR_LIT("$ ");
}

static void shell_print_errors(const char** errors, size_t count) {
	for(size_t i = 0; i < count; ++i) {

		int res = fprintf(stderr, "%s", errors[i]);
		if(res < 0) {
			fflush(stderr);
			// Note: we already have an error, so no reporting possible here
			return;
		}
	}

	int res = fprintf(stderr, "\n");
	if(res < 0) {
		// Note: we already have an error, so no reporting possible here
	}

	fflush(stderr);
}

static void shell_print_error(const char* error) {
	const char* errors[] = { error };
	shell_print_errors(errors, ARRAY_SIZE(errors));
}

static void shell_exit(ShellInstanceState* state, ExitCode code) {
	if(state->alive) {
		// ignore duplicate exits, the first one sets the code, the second one is
		// ignore (eg if we call "exit 22" and get EOF after that, it should result
		// in 22)
		state->alive = false;
		state->result = code;
	}
}

static void shell_global_exit(ShellState* state, ExitCode code) {
	shell_exit(&(state->global), code);
}

static void shell_virtual_fn_sync_exit(shell_virtual_context ctx, const ExitData* data) {

	ShellState* state = ctx;

	if(!data->has_code) {

		RETURNS_VOID shell_global_exit(state, state->command_state.exit_code);
	} else {
		RETURNS_VOID shell_global_exit(state, data->code);
	}
}

static tstr_view shell_virtual_fn_sync_get_current_working_directory(shell_virtual_context ctx) {

	ShellState* state = ctx;

	return tstr_as_view(&(state->global.cwd));
}

static bool shell_virtual_fn_sync_set_current_working_directory(shell_virtual_context ctx,
                                                                const tstr* normalized_cwd) {

	ShellState* state = ctx;

	if(!path_is_absolute(tstr_as_view(normalized_cwd))) {
		return false;
	}

	if(!path_is_normalized(tstr_as_view(normalized_cwd))) {
		return false;
	}

	if(!path_exists_and_is_dir(tstr_as_view(normalized_cwd))) {
		return false;
	}

	int res = chdir(tstr_cstr(normalized_cwd));
	if(res < 0) {
		return false;
	}

	tstr_free(&(state->global.cwd));
	state->global.cwd = tstr_dup(normalized_cwd);

	return true;
}

static ShellVirtualFunctions g_shell_functions_synchronous = {
	.exit = shell_virtual_fn_sync_exit,
	.get_current_working_directory = shell_virtual_fn_sync_get_current_working_directory,
	.set_current_working_directory = shell_virtual_fn_sync_set_current_working_directory,
};

#define SHELL_HANDLE_SYNCHRONOUS(ctx) \
	{ .context = (ctx), .function = g_shell_functions_synchronous };

typedef struct {
	ShellInstanceState instance;
} ShellSubShellState;

static void shell_virtual_fn_subshell_exit(shell_virtual_context ctx, const ExitData* data) {

	ShellSubShellState* state = ctx;

	if(!data->has_code) {
		// TODO: add this to the instance state, once we support "&&" "||" and ";"
		// alias multiple commands in one subshell
		// RETURNS_VOID shell_exit(state, state->command_state.exit_code);
		RETURNS_VOID shell_exit(&(state->instance), ExitCodeSuccess);
	} else {
		RETURNS_VOID shell_exit(&(state->instance), data->code);
	}
}

static tstr_view
shell_virtual_fn_subshell_get_current_working_directory(shell_virtual_context ctx) {

	ShellSubShellState* state = ctx;

	return tstr_as_view(&(state->instance.cwd));
}

static bool shell_virtual_fn_subshell_set_current_working_directory(shell_virtual_context ctx,
                                                                    const tstr* normalized_cwd) {

	ShellSubShellState* state = ctx;

	if(!path_is_absolute(tstr_as_view(normalized_cwd))) {
		return false;
	}

	if(!path_is_normalized(tstr_as_view(normalized_cwd))) {
		return false;
	}

	if(!path_exists_and_is_dir(tstr_as_view(normalized_cwd))) {
		return false;
	}

	int res = chdir(tstr_cstr(normalized_cwd));
	if(res < 0) {
		return false;
	}

	tstr_free(&(state->instance.cwd));
	state->instance.cwd = tstr_dup(normalized_cwd);

	return true;
}

static ShellVirtualFunctions g_shell_functions_subshell = {
	.exit = shell_virtual_fn_subshell_exit,
	.get_current_working_directory = shell_virtual_fn_subshell_get_current_working_directory,
	.set_current_working_directory = shell_virtual_fn_subshell_set_current_working_directory,
};

#define SHELL_HANDLE_SUBSHELL(ctx) { .context = (ctx), .function = g_shell_functions_subshell };

#if SHELL_USE_EDIT_LIBRARY == 0
static void shell_cleanup_old_prompt(ShellState* state) {
	tstr_free(&state->input_state.last_prompt_string);
}
static void shell_set_new_prompt(ShellState* state, tstr prompt) {
	shell_cleanup_old_prompt(state);
	state->input_state.last_prompt_string = prompt;
}

static bool shell_process_input_state(ShellState* state) {
	shell_cleanup_old_prompt(state);
	return true;
}
#elif SHELL_USE_EDIT_LIBRARY == 1
static bool shell_set_prompt_to_input_state(ShellState* state) {
	tstr prompt;
	if(state->interactive) {
		prompt = shell_get_prompt(state);

		if(tstr_is_null(&prompt)) {
			RETURNS_VOID shell_global_exit(state, ExitCodeGenericError);

			RETURNS_VOID shell_print_error("[INTERNAL] prompt print error");
			return false;
		}
	} else {
		prompt = tstr_null();
	}

	state->input_state.prompt = prompt;

	return true;
}

static bool shell_process_input_state(ShellState* state) {

	return shell_set_prompt_to_input_state(state);
}
#else
static bool shell_print_prompt(ShellState* state) {
	tstr prompt;
	if(state->interactive) {
		prompt = shell_get_prompt(state);

		if(tstr_is_null(&prompt)) {
			RETURNS_VOID shell_global_exit(state, ExitCodeGenericError);

			RETURNS_VOID shell_print_error("[INTERNAL] prompt print error");
			return false;
		}
	} else {
		prompt = tstr_null();
	}

	int res = fprintf(stdout, TSTR_FMT, TSTR_FMT_ARGS(prompt));

	tstr_free(&prompt);

	if(res < 0) {
		return false;
	}

	return true;
}

static bool shell_process_input_state(ShellState* state) {

	return shell_print_prompt(state);
}
#endif

static void set_command_exit_code(ShellState* state, ExitCode code) {
	state->command_state.exit_code = code;
}

static void shell_execute_command_synchronous(const ShellCommand* const command,
                                              ShellState* state) {
	switch(command->type) {
		case ShellCommandTypeBuiltin: {
			const ShellCommandBuiltin* builtin = &(command->data.builtin);

			ShellHandle shell_handle = SHELL_HANDLE_SYNCHRONOUS(state);

			ExitCode command_exit_code = invoke_shell_builtin_in_current_context(
			    builtin, shell_handle, &(command->settings));
			set_command_exit_code(state, command_exit_code);

			builtin->command.validator.free(builtin->args);

			break;
		}
		case ShellCommandTypeExecutable: {
			const ShellCommandExecutable executable = command->data.executable;

			ExitCode command_exit_code = invoke_executable_synchronous(
			    &executable.path, &executable.invoke_name, &(command->args), &(command->settings));
			set_command_exit_code(state, command_exit_code);

			break;
		}
		default: {
			RETURNS_VOID shell_global_exit(state, ExitCodeImplementationError);

			RETURNS_VOID shell_print_error("[INTERNAL] Invalid command type");
			break;
		}
	}
}

typedef struct {
	ShellSubShellState state;
	const ShellCommandBuiltin* builtin;
	const ShellRedirectSettings* settings;
	const ShellPipeSettings* pipe_settings;
} ShellSubShellThreadState;

static void* shell_subshell_builtin_thread_entry(void* arg) {

	ShellSubShellThreadState* state = arg;

	ShellSubShellState* subshell_state = &(state->state);

	ShellHandle shell_handle = SHELL_HANDLE_SUBSHELL(subshell_state);

	ExitCode command_exit_code = invoke_shell_builtin_in_subshell(
	    state->builtin, shell_handle, state->settings, state->pipe_settings);

	shell_exit(&(subshell_state->instance), command_exit_code);

	state->builtin->command.validator.free(state->builtin->args);
	return NULL;
}

typedef enum : uint8_t {
	ShellCommandSynchronizationTypeError = 0,
	ShellCommandSynchronizationTypeThread,
	ShellCommandSynchronizationTypeProcess,
} ShellCommandSynchronizationType;

typedef struct {
	pthread_t thread_id;
	ShellSubShellThreadState* state;
} ShellCommandSynchronizationThread;

typedef struct {
	ShellExecuteAsyncHandle handle;
} ShellCommandSynchronizationProcess;

typedef struct {
	ShellCommandSynchronizationType type;
	union {
		tstr error;
		ShellCommandSynchronizationThread thread;
		ShellCommandSynchronizationProcess process;
	} data;
} ShellCommandSynchronization;

static void shell_destroy_instance_state(ShellInstanceState* state) {

	tstr_free(&(state->cwd));
}

static void free_sub_shell_thread_state(ShellSubShellThreadState* state) {

	shell_destroy_instance_state(&state->state.instance);

	free(state);
}

static void shell_execute_command_in_subshell(const ShellCommand* const command,
                                              const ShellState* state,
                                              const ShellPipeSettings* pipe_settings,
                                              ShellCommandSynchronization* result) {
	switch(command->type) {
		case ShellCommandTypeBuiltin: {
			const ShellCommandBuiltin* builtin = &(command->data.builtin);

			ShellSubShellThreadState* thread_state = malloc(sizeof(ShellSubShellThreadState));

			if(thread_state == NULL) {
				*result =
				    (ShellCommandSynchronization){ .type = ShellCommandSynchronizationTypeError,
					                               .data = { .error = TSTR_LIT("OOM error") } };
				return;
			}

			tstr cwd = tstr_dup(&state->global.cwd);

			if(tstr_is_null(&cwd)) {
				*result =
				    (ShellCommandSynchronization){ .type = ShellCommandSynchronizationTypeError,
					                               .data = { .error = TSTR_LIT("cwd get error") } };
				return;
			}

			*thread_state =
        (ShellSubShellThreadState){.state =
                                       {
                                           .instance =
                                               {
                                                   .alive = true,
                                                   .result = ExitCodeSuccess,
                                                   .cwd = cwd,
                                               },
                                       },
                                   .builtin = builtin,
                                   .settings = &(command->settings),
                                   .pipe_settings = pipe_settings};

			pthread_t thread_id;

			int res =
			    pthread_create(&thread_id, NULL, shell_subshell_builtin_thread_entry, thread_state);

			if(res != 0) {
				*result =
				    (ShellCommandSynchronization){ .type = ShellCommandSynchronizationTypeError,
					                               .data = { .error = TSTR_LIT("pthread error") } };
				return;
			}

			*result = (ShellCommandSynchronization){
				.type = ShellCommandSynchronizationTypeThread,
				.data = { .thread = { .thread_id = thread_id, .state = thread_state } }
			};
			return;
		}
		case ShellCommandTypeExecutable: {
			const ShellCommandExecutable executable = command->data.executable;

			ShellExecuteAsyncResult res =
			    invoke_executable_async(&executable.path, &executable.invoke_name, &(command->args),
			                            &(command->settings), pipe_settings);

			if(!res.is_ok) {
				*result =
				    (ShellCommandSynchronization){ .type = ShellCommandSynchronizationTypeError,
					                               .data = { .error = res.data.error } };
				return;
			}

			*result = (ShellCommandSynchronization){ .type = ShellCommandSynchronizationTypeProcess,
				                                     .data = { .process = {
				                                                   .handle = res.data.handle } } };
			return;
		}
		default: {
			UNREACHABLE();
			break;
		}
	}
}

TVEC_DEFINE_AND_IMPLEMENT_VEC_TYPE(ShellPipeSettings)

typedef TVEC_TYPENAME(ShellPipeSettings) ShellPipeSettingsArray;

TVEC_DEFINE_AND_IMPLEMENT_VEC_TYPE(ShellCommandSynchronization)

typedef TVEC_TYPENAME(ShellCommandSynchronization) ShellCommandSynchronizationArray;

static void shell_execute_pipeline(const ShellParseDataPipeline* pipeline, ShellState* state) {

	const size_t pipeline_length = TVEC_LENGTH(ShellCommand, pipeline->commands);

	if(pipeline_length == 0) {
		RETURNS_VOID shell_global_exit(state, ExitCodeImplementationError);

		RETURNS_VOID shell_print_error("[INTERNAL] Invalid empty pipeline");
		return;
	}

	// do this synchronously, as we don't need complicated stuff for just one
	// command
	if(pipeline_length == 1) {
		const ShellCommand* const command = TVEC_GET_AT(ShellCommand, &(pipeline->commands), 0);
		ASSERT(command != NULL);

		RETURNS_VOID shell_execute_command_synchronous(command, state);
		return;
	}

	// setup pipeline and sync state
	// we need an array for pipes, and than a array to hold sync states (either
	// for the subshells thread of the builtin or subshell pids of the child
	// processes for executables)
	ShellPipeSettingsArray pipe_settings = TVEC_EMPTY(ShellPipeSettings);
	ShellCommandSynchronizationArray synchronization = TVEC_EMPTY(ShellCommandSynchronization);

	TvecResult res =
	    TVEC_ALLOCATE_UNINITIALIZED(ShellPipeSettings, &pipe_settings, pipeline_length);

	if(res != TvecResultOk) { // NOLINT(readability-implicit-bool-conversion)
		RETURNS_VOID shell_global_exit(state, ExitCodeImplementationError);

		RETURNS_VOID shell_print_error("OOM in executing pipeline");
		return;
	}

	TvecResult res2 =
	    TVEC_ALLOCATE_UNINITIALIZED(ShellCommandSynchronization, &synchronization, pipeline_length);

	if(res2 != TvecResultOk) { // NOLINT(readability-implicit-bool-conversion)
		RETURNS_VOID shell_global_exit(state, ExitCodeImplementationError);

		RETURNS_VOID shell_print_error("OOM in executing pipeline");
		return;
	}

	{
		ShellPipeState next_pipe = { .fds = { 0, 0 } };

		for(size_t i = 0; i < pipeline_length; ++i) {

			ShellPipeSettings* const pipe_setting =
			    TVEC_GET_AT_MUT(ShellPipeSettings, &pipe_settings, i);
			ASSERT(pipe_setting != NULL);
			ShellCommandSynchronization* const sync =
			    TVEC_GET_AT_MUT(ShellCommandSynchronization, &synchronization, i);
			ASSERT(sync != NULL);

			*sync =
			    (ShellCommandSynchronization){ .type = ShellCommandSynchronizationTypeError,
				                               .data = { .error = TSTR_LIT("Not filled yet") } };

			*pipe_setting = (ShellPipeSettings){
				.stdin = { .is_pipe = false, .state = { .fds = { 0, 0 } } },
				.stdout = { .is_pipe = false, .state = { .fds = { 0, 0 } } },
			};

			ShellPipeState current_pipe = { .fds = { 0, 0 } };

			if(i < (pipeline_length - 1)) {

				int res = pipe(current_pipe.fds);

				if(res != 0) {
					RETURNS_VOID shell_global_exit(state, ExitCodeImplementationError);

					RETURNS_VOID shell_print_error("error in creating pipe");
					goto pipeline_cleanup;
				}

				pipe_setting->stdout = (ShellMaybePipe){ .is_pipe = true, .state = current_pipe };
			}

			if(i > 0) {
				pipe_setting->stdin = (ShellMaybePipe){ .is_pipe = true, .state = next_pipe };
			}

			next_pipe = current_pipe;
		}
	}

	for(size_t i = 0; i < TVEC_LENGTH(ShellCommand, pipeline->commands); ++i) {

		const ShellCommand* const command = TVEC_GET_AT(ShellCommand, &(pipeline->commands), i);
		ASSERT(command != NULL);
		ShellCommandSynchronization* const sync =
		    TVEC_GET_AT_MUT(ShellCommandSynchronization, &synchronization, i);
		ASSERT(sync != NULL);
		const ShellPipeSettings* const pipe_setting =
		    TVEC_GET_AT(ShellPipeSettings, &pipe_settings, i);
		ASSERT(pipe_setting != NULL);

		// if we have a executable, launch it with the pipe settings and get the pid
		// for a builtin start a new thread and get the thread_id
		RETURNS_VOID shell_execute_command_in_subshell(command, state, pipe_setting, sync);

		if(sync->type == ShellCommandSynchronizationTypeError) {
			RETURNS_VOID shell_global_exit(state, ExitCodeImplementationError);

			const char* errors[] = { "[INTERNAL]: ", tstr_cstr(&(sync->data.error)) };
			RETURNS_VOID shell_print_errors(errors, ARRAY_SIZE(errors));
			tstr_free(&(sync->data.error));

			goto pipeline_cleanup;
		}
		//
	}

	// TODO(Totto): here we should implement pipefail, when we need to

	// wait for all steps of the pipeline to end

	for(size_t i = 0; i < TVEC_LENGTH(ShellCommand, pipeline->commands); ++i) {

		// just walk linearly through all of them we need to wait nevertheless for
		// all, so might as well do left to right in a pipline, no nened for fancy
		// waitpid(WNOHANG) or condition variables for threads

		ShellCommandSynchronization* const sync =
		    TVEC_GET_AT_MUT(ShellCommandSynchronization, &synchronization, i);
		ASSERT(sync != NULL);
		const ShellPipeSettings* const pipe_setting =
		    TVEC_GET_AT(ShellPipeSettings, &pipe_settings, i);
		ASSERT(pipe_setting != NULL);

		switch(sync->type) {
			case ShellCommandSynchronizationTypeProcess: {
				int status;
				int res = waitpid(sync->data.process.handle.child_pid, &status, 0);
				if(res < 0) {
					RETURNS_VOID shell_global_exit(state, ExitCodeImplementationError);

					RETURNS_VOID shell_print_error("error in waitpid");
					goto pipeline_cleanup;
				}

				if(WIFEXITED(status)) {
					uint8_t exit_status = WEXITSTATUS(status);
					// this is only needed for pipefail
					UNUSED(exit_status);
				} else if(WIFSIGNALED(status)) {
					uint8_t signal_number = WTERMSIG(status);
					// this is only needed for pipefail
					UNUSED(signal_number);
				} else {
					RETURNS_VOID shell_global_exit(state, ExitCodeImplementationError);

					RETURNS_VOID shell_print_error("process not exited");
					goto pipeline_cleanup;
				}

				// handle pipes, these need to be cleaned up in this scenario only and
				// after the processes, as the fork duplicates teh pipe fds and they need
				// to be closed in the forked and this process

				if(pipe_setting->stdout.is_pipe) {

					// close the unnecessary WRITE_END
					IGNORE_RESULT close(pipe_setting->stdout.state.fds[PIPE_WRITE_END]);
				}

				if(pipe_setting->stdin.is_pipe) {

					// close the unnecessary READ_END
					IGNORE_RESULT close(pipe_setting->stdout.state.fds[PIPE_READ_END]);
				}

				break;
			}
			case ShellCommandSynchronizationTypeThread: {

				void* return_value = NULL;
				int res = pthread_join(sync->data.thread.thread_id, &return_value);

				if(res != 0) {
					RETURNS_VOID shell_global_exit(state, ExitCodeImplementationError);

					RETURNS_VOID shell_print_error("process not exited");
					goto pipeline_cleanup;
				}

				RETURNS_VOID free_sub_shell_thread_state(sync->data.thread.state);
				break;
			}
			case ShellCommandSynchronizationTypeError: {
				RETURNS_VOID shell_global_exit(state, ExitCodeImplementationError);

				const char* errors[] = { "[INTERNAL]: ", tstr_cstr(&sync->data.error) };
				RETURNS_VOID shell_print_errors(errors, ARRAY_SIZE(errors));

				tstr_free(&sync->data.error);
				goto pipeline_cleanup;
			}
			default: {
				RETURNS_VOID shell_global_exit(state, ExitCodeImplementationError);

				RETURNS_VOID shell_print_error("[INTERNAL] invalid shell command sync type");
				goto pipeline_cleanup;
			}
		}
	}

pipeline_cleanup:
	// cleanup pipes, if they are forked, we need to close them here, otherwise we
	// need to cleanup after the builtins

	TVEC_FREE(ShellCommandSynchronization, &synchronization);
	TVEC_FREE(ShellPipeSettings, &pipe_settings);
}

void shell_repl_step(ShellState* state) {

	if(!shell_process_input_state(state)) {

		RETURNS_VOID shell_global_exit(state, ExitCodeGenericError);

		RETURNS_VOID shell_print_error("[INTERNAL] prompt print error");
		return;
	}

	ShellInputResult input = shell_get_input(&(state->input_state));

	if(input.type == ShellInputResultTypeEOF) {
		RETURNS_VOID shell_global_exit(state, state->command_state.exit_code);
		return;
	}

	if(input.type != ShellInputResultTypeOk) {
		tstr error = TSTR_LIT("Can't read input");
		if(input.type == ShellInputResultTypeError) {
			tstr_free(&error);
			error = input.data.error;
		}

		RETURNS_VOID shell_global_exit(state, ExitCodeImplementationError);

		RETURNS_VOID shell_print_error(tstr_cstr(&error));
		tstr_free(&error);
		return;
	}

	ShellHandle shell_handle = SHELL_HANDLE_SYNCHRONOUS(state);

	ShellInputOkResult data = input.data.ok;
	ShellParseDataResult parse = shell_parse_data(INPUT_DATA_VIEW(data), shell_handle);

	switch(parse.type) {
		case ShellParseDataResultTypePipeline: {
			const ShellParseDataPipeline pipeline = parse.data.pipeline;

			RETURNS_VOID shell_execute_pipeline(&pipeline, state);

			free_shell_pipeline(&parse.data.pipeline);

			break;
		}
		case ShellParseDataResultTypeCommandsNotFound: {
			const ShellCommandsNotFound not_found = parse.data.not_found;

			for(size_t i = 0; i < TVEC_LENGTH(ShellCommandNotFound, not_found.array); ++i) {

				const ShellCommandNotFound* const command_not_found =
				    TVEC_GET_AT(ShellCommandNotFound, &(not_found.array), i);
				ASSERT(command_not_found != NULL);

				int res = fprintf(stderr, TSTR_FMT ": command not found\n",
				                  TSTR_FMT_ARGS(command_not_found->name));

				if(res < 0) {
					RETURNS_VOID shell_global_exit(state, ExitCodeIOError);

					RETURNS_VOID shell_print_error("[INTERNAL] output error");

					goto err_out_of_loop;
				}
			}

			set_command_exit_code(state, ExitCodeCommandNotFound);

		err_out_of_loop:
			free_shell_commands_not_found(&parse.data.not_found);

			break;
		}
		case ShellParseDataResultTypeEmpty: {
			break;
		}
		case ShellParseDataResultTypeError: {
			int res = fprintf(stderr, TSTR_FMT "\n", TSTR_FMT_ARGS(parse.data.error));
			tstr_free(&parse.data.error);

			if(res < 0) {
				RETURNS_VOID shell_global_exit(state, ExitCodeIOError);

				RETURNS_VOID shell_print_error("[INTERNAL] output error");
				break;
			}
			set_command_exit_code(state, ExitCodeGenericError);
			break;
		}
		default: {
			RETURNS_VOID shell_global_exit(state, ExitCodeImplementationError);

			RETURNS_VOID shell_print_error("[INTERNAL] Invalid parse result");
			break;
		}
	}

#if SHELL_USE_EDIT_LIBRARY == 2
	if(data.end) {
		RETURNS_VOID shell_global_exit(state, state->command_state.exit_code);
	}
#endif

	RETURNS_VOID free_shell_input_ok_result(data);
}

typedef enum : uint8_t {
	ShellSetupResultTypeOk = 0,
	ShellSetupResultTypeError,
} ShellSetupResultType;

typedef struct {
	ShellSetupResultType type;
	union {
		ShellState ok;
		tstr error;
	} data;
} ShellSetupResult;

static ShellSetupResult shell_setup_repl(const char* const program_name) {
	// Flush after every printf
	RETURNS_VOID setbuf(stdout, NULL);

#if SHELL_USE_EDIT_LIBRARY == 0
	EditLine* el = el_init(program_name, stdin, stdout, stderr);
	if(el == NULL) {
		return (ShellSetupResult){ .type = ShellSetupResultTypeError,
			                       .data = { .error = TSTR_LIT("libedit init error") } };
	}

	// TODO: setup custom libedit behaviour
	//  this includes: signal handling, tab completion, history

	ShellInputState input_state = { .el = el };
#elif SHELL_USE_EDIT_LIBRARY == 1
	UNUSED(program_name);
	int res = rl_initialize();
	if(res != 0) {
		return (ShellSetupResult){ .type = ShellSetupResultTypeError,
			                       .data = { .error = TSTR_LIT("readline init error") } };
	}

	// TODO: setup custom readline behaviour
	//  this includes: signal handling, tab completion, history

	ShellInputState input_state = { .prompt = tstr_null() };
#else
	UNUSED(program_name);
	ShellInputState input_state = {};
#endif

	bool interactive = isatty(STDIN_FILENO) == 1;

	tstr cwd = path_get_cwd();

	if(tstr_is_null(&cwd)) {
		return (ShellSetupResult){ .type = ShellSetupResultTypeError,
			                       .data = { .error = TSTR_LIT("cwd get error") } };
	}

	ShellState state = {
      .command_state = {.exit_code = ExitCodeSuccess},
      .interactive = interactive,
      .input_state = input_state,
      .global =
          {
              .alive = true,
              .result = ExitCodeSuccess,
              .cwd = cwd,
          },
  };

	return (ShellSetupResult){ .type = ShellSetupResultTypeOk, .data = { .ok = state } };
}

static void shell_destroy_state(ShellState* state) {

	shell_destroy_instance_state(&(state->global));

#if SHELL_USE_EDIT_LIBRARY == 0
	el_end(state->input_state.el);
	shell_cleanup_old_prompt(state);
#endif
}

#if SHELL_USE_EDIT_LIBRARY == 0
typedef const char* (*el_pfunct_t)(EditLine*);

NODISCARD static bool el_set_prompt(EditLine* e, el_pfunct_t fn) {
	return el_set(e, EL_PROMPT, fn) == 0;
}

NODISCARD static bool el_set_clientdata(EditLine* e, void* data) {
	return el_set(e, EL_CLIENTDATA, data) == 0;
}

NODISCARD static void* el_get_clientdata(EditLine* e) {
	void* data = NULL;
	int res = el_get(e, EL_CLIENTDATA, &data);
	if(res != 0) {
		return NULL;
	}

	return data;
}

NODISCARD static const char* shell_get_prompt_el_cb(EditLine* el) {

	ShellState* state = el_get_clientdata(el);
	if(state == NULL) {
		goto err;
	}

	tstr prompt;
	if(state->interactive) {
		prompt = shell_get_prompt(state);

		if(tstr_is_null(&prompt)) {
			goto err;
		}
	} else {
		prompt = TSTR_LIT("");
	}

	const char* prompt_str = tstr_cstr(&prompt);

	if(prompt_str == NULL) {
		goto err;
	}

	shell_set_new_prompt(state, prompt);

	return prompt_str;

err:
	RETURNS_VOID shell_global_exit(state, ExitCodeImplementationError);

	RETURNS_VOID shell_print_error("[INTERNAL] prompt print error");
	return "$ ";
}
#endif

NODISCARD static bool shell_setup_prompt(ShellState* state) {

#if SHELL_USE_EDIT_LIBRARY == 0
	bool ok = el_set_clientdata(state->input_state.el, state);
	if(!ok) {
		return false;
	}
	ok = el_set_prompt(state->input_state.el, shell_get_prompt_el_cb);
	if(!ok) {
		return false;
	}
	// TODO: set editor, fo standard shortcuts
	// el_set(EL_EDITOR, "emacs" "vi")
	return true;
#elif SHELL_USE_EDIT_LIBRARY == 1

	return shell_set_prompt_to_input_state(state);
#else
	UNUSED(state);
	return true;
#endif
}

ExitCode shell_execute_repl(const size_t argc, char* const* const argv) {
	if(argc == 0) {
		RETURNS_VOID shell_print_error("[INTERNAL] missing argv[0]");
		return ExitCodeImplementationError;
	}

	ShellSetupResult setup_res = shell_setup_repl(argv[0]);

	if(setup_res.type != ShellSetupResultTypeOk) {
		ASSERT(setup_res.type == ShellSetupResultTypeError);
		const char* errors[] = { "[INTERNAL]: ", tstr_cstr(&setup_res.data.error) };
		RETURNS_VOID shell_print_errors(errors, ARRAY_SIZE(errors));

		tstr_free(&setup_res.data.error);

		return ExitCodeImplementationError;
	}

	ShellState state = setup_res.data.ok;

	bool ok = shell_setup_prompt(&state);
	if(!ok) {
		RETURNS_VOID shell_print_error("[INTERNAL]: el prompt set error");

		tstr_free(&setup_res.data.error);

		return ExitCodeImplementationError;
	}

	while(state.global.alive) {
		shell_repl_step(&state);
	}

	shell_destroy_state(&state);

	return state.global.result;
}
