#ifndef _GNU_SOURCE
	#define _GNU_SOURCE
	#define SHELL_NEEDED_GNU_SOURCE
#endif
#include <pthread.h> // for pthread_tryjoin_np
#ifdef SHELL_NEEDED_GNU_SOURCE
	#undef SHELL_NEEDED_GNU_SOURCE
	#undef _GNU_SOURCE
#endif

#include "./shell.h"

#include <dirent.h>
#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <tmap/tmap.h>
#include <tstr/tstr.h>

#include "./builtins.h"
#include "./executable.h"
#include "./file.h"
#include "./history.h"
#include "./input.h"
#include "./parse.h"
#include "./trie.h"
#include "./utils.h"

// TODO: remove
int g_is_docker = 0;

typedef struct {
	ExitCode exit_code;
} CommandState;

typedef struct {
	size_t last_append_idx;
	tstr histfile;
} ShellHistorySaveState;

TMAP_DEFINE_MAP_TYPE(tstr, TStr, ShellVariable, ShellVariableHashMap)

typedef TMAP_TYPENAME_MAP(ShellVariableHashMap) ShellVariableHashMap;

typedef struct {
	ShellVariableHashMap map;
} ShellVariableStorage;

typedef struct {
	bool alive;
	ExitCode result;
	tstr cwd;
	ShellHistory history;
	ShellHistorySaveState history_state;
	ShellVariableStorage variables;
} ShellInstanceState;

TMAP_DEFINE_MAP_TYPE(tstr, TStr, ShellCompletion, ShellCompletionHashMap)

typedef TMAP_TYPENAME_MAP(ShellCompletionHashMap) ShellCompletionHashMap;

typedef struct {
	ShellCompletionHashMap map;
} ShellCompleteStorage;

typedef struct {
	Trie* completion_trie;
	ShellCompleteStorage storage;
} ShellCompletionState;

typedef struct {
	bool debug;
} ShellEnvSettings;

typedef struct {
	ShellInstanceState instance;
} ShellSubShellState;

typedef struct {
	ShellSubShellState state;
	const ShellCommandBuiltin* builtin;
	const ShellRedirectSettings* settings;
	const ShellPipeState* pipe_state;
} ShellSubShellThreadState;

typedef struct {
	pthread_t thread_id;
	ShellSubShellThreadState* state;
} ShellCommandSynchronizationThread;

typedef struct {
	ShellExecuteAsyncHandle handle;
} ShellCommandSynchronizationProcess;

typedef enum : uint8_t {
	ShellCommandSynchronizationTypeError = 0,
	ShellCommandSynchronizationTypeThread,
	ShellCommandSynchronizationTypeProcess,
} ShellCommandSynchronizationType;

typedef struct {
	ShellCommandSynchronizationType type;
	union {
		tstr error;
		ShellCommandSynchronizationThread thread;
		ShellCommandSynchronizationProcess process;
	} data;
} ShellCommandSynchronization;

typedef struct {
	ShellCommandSynchronization sync;
	tstr command;
	bool finished;
	size_t id;
} ShellJobState;

TVEC_DEFINE_VEC_TYPE(ShellJobState)

typedef TVEC_TYPENAME(ShellJobState) ShellJobStateArray;

typedef struct {
	ShellJobStateArray array;
} ShellJobsState;

typedef struct {
	CommandState command_state;
	const bool interactive;
	ShellInputState input_state;
	ShellInstanceState global;
	ShellCompletionState completion_state;
	ShellEnvSettings settings;
	ShellJobsState jobs_state;
} ShellState;

TRTTI_DECLARE_TYPE_AS_SUPPORTED(ShellState)

TRTTI_IMPLEMENTATION_FOR_TYPE(ShellState)

static tstr shell_get_prompt(const ShellState* const state) {

	// TODO(Totto): support setting the prompt dynamically
	UNUSED(state);

	return TSTR_LIT("$ ");
}

__attribute__((format(printf, 1, 0))) static void shell_print_error_impl_va(const char* message,
                                                                            va_list args) {

	int res = vfprintf(stderr, message, args);

	if(res < 0) {
		fflush(stderr);
		// Note: we already have an error, so no reporting possible here
		goto report_error_end;
	}

	fflush(stderr);

report_error_end:
	return;
}

__attribute__((format(printf, 1, 2))) static void shell_print_error(const char* message, ...) {
	va_list args;
	va_start(args, message);

	RETURNS_VOID shell_print_error_impl_va(message, args);

	va_end(args);
}

MAYBE_UNUSED __attribute__((format(printf, 2, 3))) static void
shell_global_debug(ShellState* state, const char* message, ...) {

	if(state->settings.debug) {
		va_list args;
		va_start(args, message);

		RETURNS_VOID shell_print_error_impl_va(message, args);

		va_end(args);
	}
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

static bool shell_instance_add_to_history(ShellInstanceState* state, tstr command) {

	// NOTE: we don't handle "duplicates" (bytewise same strings)

	TvecResult res = TVEC_PUSH(tstr, &(state->history.array), command);

	if(res != TvecResultOk) { // NOLINT(readability-implicit-bool-conversion)
		tstr_free(&command);
		return false;
	}

	return true;
}

static tstr shell_instance_append_history_to_file(ShellInstanceState* state, const tstr* file) {

	const size_t length = TVEC_LENGTH(tstr, state->history.array);

	tstr result =
	    append_history_to_file(&(state->history), file, state->history_state.last_append_idx);

	if(!tstr_is_null(&result)) {
		return result;
	}

	state->history_state.last_append_idx = length;

	return tstr_null();
}

static void shell_global_exit(ShellState* state, ExitCode code) {
	shell_exit(&(state->global), code);
}

TRTTI_DECLARE_TYPE_AS_SUPPORTED(ShellSubShellThreadState)

static void free_shell_history(ShellHistory* history) {

	for(size_t i = 0; i < TVEC_LENGTH(tstr, history->array); ++i) {

		tstr* const arg = TVEC_GET_AT_MUT(tstr, &(history->array), i);
		ASSERT(arg != NULL);

		tstr_free(arg);
	}

	TVEC_FREE(tstr, &(history->array));
}

static void free_shell_variable(ShellVariable* variable) {
	tstr_free(&(variable->name));
	tstr_free(&(variable->value));
}

static void free_shell_variable_storage(ShellVariableStorage* storage) {
	TMAP_TYPENAME_ITER(ShellVariableHashMap)
	iter = TMAP_ITER_INIT(ShellVariableHashMap, &(storage->map));

	TMAP_TYPENAME_ENTRY(ShellVariableHashMap) value;

	while(TMAP_ITER_NEXT(ShellVariableHashMap, &iter, &value)) {
		tstr_free(&value.key);
		free_shell_variable(&value.value);
	}

	TMAP_FREE(ShellVariableHashMap, &(storage->map));
}

static void free_shell_instance_state(ShellInstanceState* state) {

	tstr_free(&(state->cwd));

	free_shell_history(&(state->history));
	tstr_free(&(state->history_state.histfile));
	free_shell_variable_storage(&(state->variables));
}

static void free_sub_shell_thread_state(ShellSubShellThreadState* state) {

	free_shell_instance_state(&state->state.instance);

	TRTTI_DESTROY(ShellSubShellThreadState, state);
}

static void free_shell_job_state(ShellJobState* arg) {
	// TODO: close this process with kill

	tstr_free(&arg->command);

	switch(arg->sync.type) {
		case ShellCommandSynchronizationTypeError: {
			tstr_free(&arg->sync.data.error);
			break;
		}
		case ShellCommandSynchronizationTypeThread: {
			ShellCommandSynchronizationThread* const thread = &(arg->sync.data.thread);

			int res = pthread_kill(thread->thread_id, SIGKILL);

			if(res != 0) {
				// a error occurred, we just ignore it, but we don't use join
			} else {

				void* return_value = NULL;
				res = pthread_join(thread->thread_id, &return_value);

				if(res != 0) {
					// ignore error
				} else {

					// only when the process exited,
					RETURNS_VOID free_sub_shell_thread_state(thread->state);
				}
			}

			break;
		}
		case ShellCommandSynchronizationTypeProcess: {
			ShellCommandSynchronizationProcess* const process = &(arg->sync.data.process);

			int res = kill(process->handle.child_pid, SIGKILL);

			int status = 0;
			res = waitpid(process->handle.child_pid, &status, 0);
			if(res < 0) {
				// ignore error
			}

			break;
		}
		default: {
			break;
		}
	}
}

static void free_shell_jobs_state(ShellJobsState* states) {

	for(size_t i = 0; i < TVEC_LENGTH(ShellJobState, states->array); ++i) {

		ShellJobState* arg = TVEC_GET_AT_MUT(ShellJobState, &(states->array), i);
		ASSERT(arg != NULL);

		free_shell_job_state(arg);
	}

	TVEC_FREE(ShellJobState, &(states->array));
}

static void shell_virtual_fn_sync_exit(shell_virtual_context ctx, const ExitData* data) {

	ShellState* state = TRTTI_ANNOTATED_VALUE_CAST(ShellState, ctx);

	if(!tstr_is_null(&(state->global.history_state.histfile))) {
		tstr ap_res = shell_instance_append_history_to_file(
		    &(state->global), &(state->global.history_state.histfile));

		if(!tstr_is_null(&ap_res)) {
			RETURNS_VOID shell_global_exit(state, ExitCodeGenericError);

			RETURNS_VOID shell_print_error("[INTERNAL] history file final write failed\n");
			return;
		}
	}

	if(!data->has_code) {
		RETURNS_VOID shell_global_exit(state, state->command_state.exit_code);
	} else {
		RETURNS_VOID shell_global_exit(state, data->code);
	}
}

static bool shell_add_to_input_history(const tstr* command);

static bool shell_virtual_fn_sync_add_to_history_impl(ShellState* state, tstr command) {

	// note only doing this in the global shell!
	bool ok = shell_add_to_input_history(&command);

	if(!ok) {
		return false;
	}

	return shell_instance_add_to_history(&(state->global), command);
}

static bool shell_virtual_fn_sync_add_to_history(shell_virtual_context ctx, tstr_view command) {

	ShellState* state = TRTTI_ANNOTATED_VALUE_CAST(ShellState, ctx);

	tstr arg = tstr_from_view(command);

	if(tstr_is_null(&arg)) {
		return false;
	}

	return shell_virtual_fn_sync_add_to_history_impl(state, arg);
}

static const ShellHistory* shell_virtual_fn_sync_get_history(shell_virtual_context ctx) {

	ShellState* state = TRTTI_ANNOTATED_VALUE_CAST(ShellState, ctx);

	return &(state->global.history);
}

static tstr shell_virtual_fn_sync_read_history_file_impl_cb(shell_virtual_context ctx,
                                                            tstr_view entry) {
	ShellState* state = TRTTI_ANNOTATED_VALUE_CAST(ShellState, ctx);

	tstr arg = tstr_from_view(entry);

	if(tstr_is_null(&arg)) {
		return TSTR_LIT("OOM error");
	}

	if(!shell_virtual_fn_sync_add_to_history_impl(state, arg)) {
		return TSTR_LIT("history add error");
	}

	return tstr_null();
}

static tstr shell_virtual_fn_sync_process_history_file(shell_virtual_context ctx,
                                                       const HistoryProcessData* const data) {

	ShellState* state = TRTTI_ANNOTATED_VALUE_CAST(ShellState, ctx);

	switch(data->type) {
		case HistoryProcessTypeRead: {
			return read_history_to_file(&(data->file),
			                            shell_virtual_fn_sync_read_history_file_impl_cb, ctx);
		}
		case HistoryProcessTypeAppend: {
			return shell_instance_append_history_to_file(&(state->global), &(data->file));
		}
		case HistoryProcessTypeWrite: {
			return write_history_to_file(&(state->global.history), &(data->file));
		}
		default: {
			return TSTR_LIT("invalid process type");
		}
	}
}

static const ShellCompletion* shell_get_completion_impl(ShellCompletionState* state,
                                                        const tstr* name) {

	const ShellCompletion* completion =
	    TMAP_GET(ShellCompletionHashMap, &(state->storage.map), *name);

	if(completion == NULL) {
		return NULL;
	}

	return completion;
}

static void free_shell_completion(ShellCompletion* completion) {
	tstr_free(&completion->script);
}

static bool shell_set_completion_impl(ShellCompletionState* state, tstr name,
                                      ShellCompletion completion) {

	ShellCompletion* map_entry =
	    TMAP_INSERT_SLOT(ShellCompletionHashMap, &(state->storage.map), name, false);
	ASSERT(map_entry != NULL);

	if(map_entry == TMAP_WOULD_OVERWRITE) {
		// free the entry before overwriting it, as this is the expected behaviour of set rather
		// then add, also free the name, as it is also duplicate

		map_entry = TMAP_INSERT_SLOT(ShellCompletionHashMap, &(state->storage.map), name, true);
		ASSERT(map_entry != NULL);
		ASSERT(map_entry != TMAP_WOULD_OVERWRITE);

		tstr_free(&name);
		free_shell_completion(map_entry);

		*map_entry = completion;

		return true;
	}

	*map_entry = completion;

	return true;
}

static bool shell_remove_completion_impl(ShellCompletionState* state, const tstr* name) {

	// TODO: tmap doesn't allow removal with free, so we just get the entry, if it exists we free it
	// and then remove it!

	const TMAP_TYPENAME_ENTRY(ShellCompletionHashMap)* map_entry =
	    TMAP_GET_ENTRY(ShellCompletionHashMap, &(state->storage.map), *name);

	if(map_entry == NULL) {
		// we can't remove anything, that isn't there
		return true;
	}

	// we can't free the key yet, as it is needed for checking the hash in the remove action, this
	// is the reason, why we need a better remove function in the TMAP API

	tstr key = map_entry->key;

	// this is another hack to free the tstr, as the get_entry function returns a const *, and there
	// is no APi for a mut *, so we use a &to local storage for the free function xD, this shows how
	// brokent his approach is, but it works TM
	ShellCompletion completion = map_entry->value;

	tstr_free(&(completion.script));

	TMAP_REM(ShellCompletionHashMap, &(state->storage.map), *name);

	tstr_free(&key);

	return true;
}

static const ShellVariable* shell_get_variable_impl(ShellVariableStorage* storage,
                                                    const tstr* name) {

	const ShellVariable* variable = TMAP_GET(ShellVariableHashMap, &(storage->map), *name);

	if(variable == NULL) {
		return NULL;
	}

	return variable;
}

static bool shell_is_valid_identifier(tstr_view identifier) {
	if(identifier.len == 0) {
		return false;
	}

	const char start_character = identifier.data[0];

	if(!shell_is_valid_identifier_start(start_character)) {
		return false;
	}

	for(size_t i = 1; i < identifier.len; ++i) {

		const char character = identifier.data[i];

		if(!shell_is_valid_identifier_character(character)) {
			return false;
		}
	}

	return true;
}

typedef struct {
	bool is_ok;
	union {
		tstr error;
		ShellVariable ok;
	} data;
} ShellVariableParseResult;

static ShellVariableParseResult shell_parse_variable(tstr_view arg) {

	if(arg.len == 0) {
		return (ShellVariableParseResult){ .is_ok = false,
			                               .data = { .error = TSTR_LIT("empty variable") } };
	}

	tstr_view name = arg;
	tstr_view value = TSTR_EMPTY_VIEW;

	for(size_t i = 0; i < arg.len; ++i) {

		const char character = arg.data[i];

		if(character == '=') {
			name = tstr_sub(arg, 0, i);
			if(i + 1 < arg.len) {
				value = tstr_sub_until_end(arg, i + 1);
			} else {
				value = TSTR_TSV("");
			}
			break;
		}
	}

	if(!shell_is_valid_identifier(name)) {
		return (ShellVariableParseResult){ .is_ok = false,
			                               .data = { .error = TSTR_LIT("invalid identifier") } };
	}

	tstr name_str = tstr_from_view(name);
	tstr value_str = tstr_from("");
	if(value.data != NULL) {
		value_str = tstr_from_view(value);
	}

	return (ShellVariableParseResult){ .is_ok = true,
		                               .data = { .ok = { .name = name_str,
		                                                 .value = value_str,
		                                                 .attributes = { ._unused = false } } } };
}

static bool shell_set_variable_impl(ShellVariableStorage* storage, tstr_view arg) {

	ShellVariableParseResult parse_result = shell_parse_variable(arg);

	if(!parse_result.is_ok) {
		// TODO: foward exact error
		tstr_free(&(parse_result.data.error));
		return false;
	}

	const ShellVariable variable = parse_result.data.ok;

	tstr key = tstr_dup(&variable.name);

	ShellVariable* map_entry = TMAP_INSERT_SLOT(ShellVariableHashMap, &(storage->map), key, false);
	ASSERT(map_entry != NULL);

	if(map_entry == TMAP_WOULD_OVERWRITE) {
		// free the entry before overwriting it, as this is the expected behaviour of set rather
		// then add, also free the key, as it is also duplicate

		map_entry = TMAP_INSERT_SLOT(ShellVariableHashMap, &(storage->map), key, true);
		ASSERT(map_entry != NULL);
		ASSERT(map_entry != TMAP_WOULD_OVERWRITE);

		tstr_free(&key);
		free_shell_variable(map_entry);

		*map_entry = variable;

		return true;
	}

	*map_entry = variable;

	return true;
}

static const ShellCompletion* shell_virtual_fn_sync_get_completion(shell_virtual_context ctx,
                                                                   const tstr* name) {
	ShellState* state = TRTTI_ANNOTATED_VALUE_CAST(ShellState, ctx);

	return shell_get_completion_impl(&(state->completion_state), name);
}

static bool shell_virtual_fn_sync_set_completion(shell_virtual_context ctx, tstr name,
                                                 ShellCompletion completion) {
	ShellState* state = TRTTI_ANNOTATED_VALUE_CAST(ShellState, ctx);

	return shell_set_completion_impl(&(state->completion_state), name, completion);
}

static bool shell_virtual_fn_sync_remove_completion(shell_virtual_context ctx, const tstr* name) {
	ShellState* state = TRTTI_ANNOTATED_VALUE_CAST(ShellState, ctx);

	return shell_remove_completion_impl(&(state->completion_state), name);
}

static size_t shell_virtual_fn_sync_get_jobs_size(shell_virtual_context ctx) {

	ShellState* state = TRTTI_ANNOTATED_VALUE_CAST(ShellState, ctx);

	return TVEC_LENGTH(ShellJobState, state->jobs_state.array);
}

static ShellJobInfoResult shell_global_get_job_impl(ShellState* state, size_t idx) {
	ShellJobState* const arg = TVEC_GET_AT_MUT(ShellJobState, &(state->jobs_state.array), idx);
	if(arg == NULL) {
		return (ShellJobInfoResult){ .is_ok = false,
			                         .data = { .error = TSTR_LIT("idx out of bounds") } };
	}

	const size_t length = TVEC_LENGTH(ShellJobState, state->jobs_state.array);

	ShellJobInfo info = {
		.handle_id = 0,
		.id = arg->id,
		.run_state = ShellJobRunStateRunning,
		.type = (length - 1) == idx                   ? ShellJobTypeRecent
		        : (length > 1 && (length - 2) == idx) ? ShellJobTypeRecent2
		                                              : ShellJobTypeNormal,
		.command = tstr_as_view(&(arg->command)),
	};

	switch(arg->sync.type) {
		case ShellCommandSynchronizationTypeError: {
			return (ShellJobInfoResult){ .is_ok = false,
				                         .data = { .error = arg->sync.data.error } };
		}
		case ShellCommandSynchronizationTypeThread: {
			const ShellCommandSynchronizationThread* const thread = &(arg->sync.data.thread);

			info.handle_id = thread->thread_id;

			void* return_value = NULL;
			int res = pthread_tryjoin_np(thread->thread_id, &return_value);
			if(res != 0) {
				if(res == EBUSY) {
					info.run_state = ShellJobRunStateRunning;
					break;
				} else {

					return (ShellJobInfoResult){
						.is_ok = false,
						.data = { .error = TSTR_LIT("[INTERNAL] calling pthread_tryjoin_np "
						                            "on already finished pthread") }
					};
				}
			}

			info.run_state = ShellJobRunStateDone;
			arg->finished = true;

			break;
		}
		case ShellCommandSynchronizationTypeProcess: {
			const ShellCommandSynchronizationProcess* const process = &(arg->sync.data.process);

			info.handle_id = (size_t)process->handle.child_pid;

			int status = 0;
			int res = waitpid(process->handle.child_pid, &status, WNOHANG);
			if(res < 0) {
				return (ShellJobInfoResult){
					.is_ok = false,
					.data = { .error = TSTR_LIT(
					              "[INTERNAL] calling waitpid on already exited process") }
				};
			}

			if(res == 0) {
				info.run_state = ShellJobRunStateRunning;
			} else {
				info.run_state = ShellJobRunStateDone;
				arg->finished = true;
			}

			break;
		}
		default: {
			break;
		}
	}

	return (ShellJobInfoResult){ .is_ok = true, .data = { .ok = info } };
}

static ShellJobInfoResult shell_virtual_fn_sync_get_job(shell_virtual_context ctx, size_t idx) {

	ShellState* state = TRTTI_ANNOTATED_VALUE_CAST(ShellState, ctx);

	return shell_global_get_job_impl(state, idx);
}

static void shell_global_reap_jobs_impl_only_removal(ShellState* state) {
	for(size_t i = 0; i < TVEC_LENGTH(ShellJobState, state->jobs_state.array);) {

		ShellJobState* arg = TVEC_GET_AT_MUT(ShellJobState, &(state->jobs_state.array), i);
		ASSERT(arg != NULL);

		// we just check the states, this reap doesn't check teh sync status
		if(arg->finished) {
			free_shell_job_state(arg);
			TVEC_REMOVE(ShellJobState, &(state->jobs_state.array), i);
		} else {
			++i;
		}
	}
}

static void shell_reap_background_jobs(ShellState* state) {

	const size_t jobs_size = TVEC_LENGTH(ShellJobState, state->jobs_state.array);

	for(size_t i = 0; i < jobs_size; ++i) {

		ShellJobInfoResult job_result = shell_global_get_job_impl(state, i);

		if(!job_result.is_ok) {

			RETURNS_VOID shell_global_exit(state, ExitCodeImplementationError);

			RETURNS_VOID shell_print_error("[INTERNAL] jobs get error: " TSTR_FMT "\n",
			                               TSTR_FMT_ARGS(job_result.data.error));

			tstr_free(&job_result.data.error);
			return;
		}
		const ShellJobInfo info = job_result.data.ok;

		const char marker = info.type == ShellJobTypeRecent    ? '+'
		                    : info.type == ShellJobTypeRecent2 ? '-'
		                                                       : ' ';

		if(info.run_state == ShellJobRunStateDone) {

			const char* status = "Done";

			int fres = fprintf(stdout, "[%zu]%c %-*s " TSTR_FMT "\n", info.id, marker, 24, status,
			                   TSV_FMT_ARGS(info.command));

			if(fres < 0) {
				RETURNS_VOID shell_global_exit(state, ExitCodeIOError);

				RETURNS_VOID shell_print_error("[INTERNAL] output error\n");
				return;
			}
		}
	}

	RETURNS_VOID shell_global_reap_jobs_impl_only_removal(state);
}

static void shell_virtual_fn_sync_reap_jobs(shell_virtual_context ctx) {

	ShellState* state = TRTTI_ANNOTATED_VALUE_CAST(ShellState, ctx);

	shell_global_reap_jobs_impl_only_removal(state);
}

static const ShellVariable* shell_virtual_fn_sync_get_variable(shell_virtual_context ctx,
                                                               const tstr* name) {
	ShellState* state = TRTTI_ANNOTATED_VALUE_CAST(ShellState, ctx);

	return shell_get_variable_impl(&(state->global.variables), name);
}

static bool shell_virtual_fn_sync_set_variable(shell_virtual_context ctx, tstr_view arg) {
	ShellState* state = TRTTI_ANNOTATED_VALUE_CAST(ShellState, ctx);

	return shell_set_variable_impl(&(state->global.variables), arg);
}

static tstr_view shell_virtual_fn_sync_get_current_working_directory(shell_virtual_context ctx) {

	ShellState* state = TRTTI_ANNOTATED_VALUE_CAST(ShellState, ctx);

	return tstr_as_view(&(state->global.cwd));
}

static bool shell_virtual_fn_sync_set_current_working_directory(shell_virtual_context ctx,
                                                                const tstr* normalized_cwd) {

	ShellState* state = TRTTI_ANNOTATED_VALUE_CAST(ShellState, ctx);

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

static const ShellVirtualFunctions g_shell_functions_synchronous = {
	.get_current_working_directory = shell_virtual_fn_sync_get_current_working_directory,
	.set_current_working_directory = shell_virtual_fn_sync_set_current_working_directory,
	.exit = shell_virtual_fn_sync_exit,
	.add_to_history = shell_virtual_fn_sync_add_to_history,
	.get_history = shell_virtual_fn_sync_get_history,
	.process_history_file = shell_virtual_fn_sync_process_history_file,
	.get_completion = shell_virtual_fn_sync_get_completion,
	.set_completion = shell_virtual_fn_sync_set_completion,
	.remove_completion = shell_virtual_fn_sync_remove_completion,
	.get_jobs_size = shell_virtual_fn_sync_get_jobs_size,
	.get_job = shell_virtual_fn_sync_get_job,
	.reap_jobs = shell_virtual_fn_sync_reap_jobs,
	.get_variable = shell_virtual_fn_sync_get_variable,
	.set_variable = shell_virtual_fn_sync_set_variable,
};

#define SHELL_HANDLE_SYNCHRONOUS(ctx) \
	{ .context = (ctx), .function = g_shell_functions_synchronous };

TRTTI_DECLARE_TYPE_AS_SUPPORTED(ShellSubShellState)

TRTTI_IMPLEMENTATION_FOR_TYPE(ShellSubShellState)

static void shell_virtual_fn_subshell_exit(shell_virtual_context ctx, const ExitData* data) {

	ShellSubShellState* state = TRTTI_ANNOTATED_VALUE_CAST(ShellSubShellState, ctx);

	if(!data->has_code) {
		// TODO(Totto): add this to the instance state, once we support "&&" "||" and ";"
		// alias multiple commands in one subshell
		// RETURNS_VOID shell_exit(state, state->command_state.exit_code);
		RETURNS_VOID shell_exit(&(state->instance), ExitCodeSuccess);
	} else {
		RETURNS_VOID shell_exit(&(state->instance), data->code);
	}
}

static bool shell_virtual_fn_subshell_add_to_history_impl(ShellSubShellState* state, tstr command) {
	return shell_instance_add_to_history(&(state->instance), command);
}

static bool shell_virtual_fn_subshell_add_to_history(shell_virtual_context ctx, tstr_view command) {

	ShellSubShellState* state = TRTTI_ANNOTATED_VALUE_CAST(ShellSubShellState, ctx);

	tstr arg = tstr_from_view(command);

	if(tstr_is_null(&arg)) {
		return false;
	}

	return shell_virtual_fn_subshell_add_to_history_impl(state, arg);
}

static const ShellHistory* shell_virtual_fn_subshell_get_history(shell_virtual_context ctx) {

	ShellSubShellState* state = TRTTI_ANNOTATED_VALUE_CAST(ShellSubShellState, ctx);

	return &(state->instance.history);
}

static tstr shell_virtual_fn_subshell_read_history_file_impl_cb(shell_virtual_context ctx,
                                                                tstr_view entry) {
	ShellSubShellState* state = TRTTI_ANNOTATED_VALUE_CAST(ShellSubShellState, ctx);

	tstr arg = tstr_from_view(entry);

	if(tstr_is_null(&arg)) {
		return TSTR_LIT("OOM error");
	}

	if(!shell_virtual_fn_subshell_add_to_history_impl(state, arg)) {
		return TSTR_LIT("history add error");
	}

	return tstr_null();
}

static tstr shell_virtual_fn_subshell_process_history_file(shell_virtual_context ctx,
                                                           const HistoryProcessData* const data) {

	ShellSubShellState* state = TRTTI_ANNOTATED_VALUE_CAST(ShellSubShellState, ctx);

	switch(data->type) {
		case HistoryProcessTypeRead: {
			return read_history_to_file(&(data->file),
			                            shell_virtual_fn_subshell_read_history_file_impl_cb, ctx);
		}
		case HistoryProcessTypeAppend: {
			return shell_instance_append_history_to_file(&(state->instance), &(data->file));
		}
		case HistoryProcessTypeWrite: {
			return write_history_to_file(&(state->instance.history), &(data->file));
		}
		default: {
			return TSTR_LIT("invalid process type");
		}
	}
}

static const ShellCompletion* shell_virtual_fn_subshell_get_completion(shell_virtual_context ctx,
                                                                       const tstr* name) {
	UNUSED(ctx);
	UNUSED(name);

	// Not supported in subshells yet
	return NULL;
}

static bool shell_virtual_fn_subshell_set_completion(shell_virtual_context ctx, tstr name,
                                                     ShellCompletion completion) {

	UNUSED(ctx);
	UNUSED(name);
	UNUSED(completion);

	// Not supported in subshells yet
	return false;
}

static bool shell_virtual_fn_subshell_remove_completion(shell_virtual_context ctx,
                                                        const tstr* name) {

	UNUSED(ctx);
	UNUSED(name);

	// Not supported in subshells yet
	return false;
}

static size_t shell_virtual_fn_subshell_get_jobs_size(shell_virtual_context ctx) {

	UNUSED(ctx);

	// Not supported in subshells yet
	return 0;
}

static ShellJobInfoResult shell_virtual_fn_subshell_get_job(shell_virtual_context ctx, size_t idx) {

	UNUSED(ctx);
	UNUSED(idx);

	// Not supported in subshells yet
	return (ShellJobInfoResult){ .is_ok = false,
		                         .data = { .error = TSTR_LIT("not supported in subshells") } };
}

static void shell_virtual_fn_subshell_reap_jobs(shell_virtual_context ctx) {

	UNUSED(ctx);

	// Not supported in subshells yet
	// but it's just a noop
}

static const ShellVariable* shell_virtual_fn_subshell_get_variable(shell_virtual_context ctx,
                                                                   const tstr* name) {
	ShellSubShellState* state = TRTTI_ANNOTATED_VALUE_CAST(ShellSubShellState, ctx);

	return shell_get_variable_impl(&(state->instance.variables), name);
}

static bool shell_virtual_fn_subshell_set_variable(shell_virtual_context ctx, tstr_view arg) {
	ShellSubShellState* state = TRTTI_ANNOTATED_VALUE_CAST(ShellSubShellState, ctx);

	return shell_set_variable_impl(&(state->instance.variables), arg);
}

static tstr_view
shell_virtual_fn_subshell_get_current_working_directory(shell_virtual_context ctx) {

	ShellSubShellState* state = TRTTI_ANNOTATED_VALUE_CAST(ShellSubShellState, ctx);

	return tstr_as_view(&(state->instance.cwd));
}

static bool shell_virtual_fn_subshell_set_current_working_directory(shell_virtual_context ctx,
                                                                    const tstr* normalized_cwd) {

	ShellSubShellState* state = TRTTI_ANNOTATED_VALUE_CAST(ShellSubShellState, ctx);

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

static const ShellVirtualFunctions g_shell_functions_subshell = {
	.get_current_working_directory = shell_virtual_fn_subshell_get_current_working_directory,
	.set_current_working_directory = shell_virtual_fn_subshell_set_current_working_directory,
	.exit = shell_virtual_fn_subshell_exit,
	.add_to_history = shell_virtual_fn_subshell_add_to_history,
	.get_history = shell_virtual_fn_subshell_get_history,
	.process_history_file = shell_virtual_fn_subshell_process_history_file,
	.get_completion = shell_virtual_fn_subshell_get_completion,
	.set_completion = shell_virtual_fn_subshell_set_completion,
	.remove_completion = shell_virtual_fn_subshell_remove_completion,
	.get_jobs_size = shell_virtual_fn_subshell_get_jobs_size,
	.get_job = shell_virtual_fn_subshell_get_job,
	.reap_jobs = shell_virtual_fn_subshell_reap_jobs,
	.get_variable = shell_virtual_fn_subshell_get_variable,
	.set_variable = shell_virtual_fn_subshell_set_variable,
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

			RETURNS_VOID shell_print_error("[INTERNAL] prompt print error\n");
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

			RETURNS_VOID shell_print_error("[INTERNAL] prompt print error\n");
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

static void shell_execute_command_synchronous(ShellState* state,
                                              const ShellCommand* const command) {
	switch(command->type) {
		case ShellCommandTypeBuiltin: {
			const ShellCommandBuiltin* builtin = &(command->data.builtin);

			RTTIAnnotatedValue state_rtti = TRTTI_ANNOTATED_VALUE_GET(ShellState, state);
			ShellHandle shell_handle = SHELL_HANDLE_SYNCHRONOUS(state_rtti);

			ExitCode command_exit_code = invoke_shell_builtin_in_current_context(
			    builtin, shell_handle, &(command->settings.redirect));
			set_command_exit_code(state, command_exit_code);

			builtin->command.validator.free(builtin->args);

			break;
		}
		case ShellCommandTypeExecutable: {
			const ShellCommandExecutable executable = command->data.executable;

			ShellExecuteSyncResult execute_res =
			    invoke_executable_synchronous(&executable.path, &executable.invoke_name,
			                                  &(command->args), &(command->settings.redirect));

			switch(execute_res.type) {
				case ShellExecuteSyncResultTypeExit: {
					set_command_exit_code(state, execute_res.data.exit);
					break;
				}
				case ShellExecuteSyncResultTypeSignal: {
					const ShellSignal signal = execute_res.data.signal;
					RETURNS_VOID shell_print_error("Process exited with signal <%u>: %s\n", signal,
					                               strsignal(signal));

					// TODO(Totto): is this correct, can we just use it as uint8_t or do we need to
					// add 128 for consitency with bash?
					set_command_exit_code(state, signal);
					break;
				}
				case ShellExecuteSyncResultTypeError: {

					RETURNS_VOID shell_print_error("Execute error: " TSTR_FMT "\n",
					                               TSTR_FMT_ARGS(execute_res.data.error));
					tstr_free(&(execute_res.data.error));

					break;
				}
				default: {
					RETURNS_VOID shell_global_exit(state, ExitCodeImplementationError);

					RETURNS_VOID shell_print_error("[INTERNAL] Invalid execute result type\n");
					break;
				}
			}

			break;
		}
		default: {
			RETURNS_VOID shell_global_exit(state, ExitCodeImplementationError);

			RETURNS_VOID shell_print_error("[INTERNAL] Invalid command type\n");
			break;
		}
	}
}

TRTTI_IMPLEMENTATION_FOR_TYPE(ShellSubShellThreadState)

static void* shell_subshell_builtin_thread_entry(RTTIAnnotatedPtr arg) {

	ShellSubShellThreadState* state = TRTTI_ANNOTATED_PTR_CAST(ShellSubShellThreadState, arg);

	ShellSubShellState* subshell_state = &(state->state);

	RTTIAnnotatedValue subshell_state_rtti =
	    TRTTI_ANNOTATED_VALUE_GET(ShellSubShellState, subshell_state);
	ShellHandle shell_handle = SHELL_HANDLE_SUBSHELL(subshell_state_rtti);

	ExitCode command_exit_code = invoke_shell_builtin_in_subshell(
	    state->builtin, shell_handle, state->settings, state->pipe_state);

	shell_exit(&(subshell_state->instance), command_exit_code);

	state->builtin->command.validator.free(state->builtin->args);
	return NULL;
}

static void shell_execute_command_in_subshell(const ShellState* state,
                                              const ShellCommand* const command,

                                              const ShellPipeState* pipe_state,
                                              ShellCommandSynchronization* result) {
	switch(command->type) {
		case ShellCommandTypeBuiltin: {
			const ShellCommandBuiltin* builtin = &(command->data.builtin);

			ShellSubShellThreadState* thread_state = TRTTI_ALLOC(ShellSubShellThreadState);

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
                                                   .history = { .array = TVEC_EMPTY(tstr) },
												   .history_state = {.last_append_idx = 0, .histfile = tstr_null()},
												   .variables = { .map = TMAP_EMPTY(ShellVariableHashMap) },
                                               },
                                       },
                                   .builtin = builtin,
                                   .settings = &(command->settings.redirect),
                                   .pipe_state = pipe_state};

			// TODO(Totto): should we read history state for subshells too?
			//  probably yes, but we have no tests, that demand that

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
			                            &(command->settings.redirect), pipe_state);

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

static void shell_execute_command_as_background_job(ShellState* state,
                                                    const ShellCommand* const command) {

	ShellPipeInfoArray pipe_info_array = TVEC_EMPTY(ShellPipeInfo);

	const ShellPipeState pipe_state = { .array = &pipe_info_array,
		                                .close = { .start = 0, .end = 0 },
		                                .settings = {
		                                    .stdin = { .is_pipe = false, .idx = 0 },
		                                    .stdout = { .is_pipe = false, .idx = 0 },
		                                } };
	ShellCommandSynchronization sync = { .type = ShellCommandSynchronizationTypeError,
		                                 .data = { .error = TSTR_LIT("Not filled yet") } };

	RETURNS_VOID shell_execute_command_in_subshell(state, command, &pipe_state, &sync);

	TVEC_FREE(ShellPipeInfo, &pipe_info_array);

	if(sync.type == ShellCommandSynchronizationTypeError) {
		RETURNS_VOID shell_global_exit(state, ExitCodeImplementationError);

		RETURNS_VOID shell_print_error("[INTERNAL]: " TSTR_FMT "\n",
		                               TSTR_FMT_ARGS(sync.data.error));
		tstr_free(&(sync.data.error));
		return;
	}

	tstr job_command = tstr_from("");

	{
		TStrResult str_res = tstr_fmt(&job_command, TSTR_FMT, TSTR_FMT_ARGS(command->arg0.parsed));

		if(str_res != TStrResultOk) { /* NOLINT(readability-implicit-bool-conversion)*/
			goto err_formatted_command;
		}
		for(size_t i = 0; i < TVEC_LENGTH(ShellArgument, command->args); ++i) {

			const ShellArgument* const arg = TVEC_GET_AT(ShellArgument, &(command->args), i);
			ASSERT(arg != NULL);

			str_res = tstr_fmt(&job_command, " " TSTR_FMT, TSTR_FMT_ARGS(arg->parsed));

			if(str_res != TStrResultOk) { /* NOLINT(readability-implicit-bool-conversion)*/
				goto err_formatted_command;
			}
		}

		goto formatted_command_ok;

	err_formatted_command:
		job_command = TSTR_LIT("<Internal Error>");
	formatted_command_ok:
	}

	ShellJobState job = {
		.sync = sync,
		.command = job_command,
		.finished = false,
		.id = 0,
	};

	const size_t job_length = TVEC_LENGTH(ShellJobState, state->jobs_state.array);

	// NOTE: the job number is one, if the array is empty, otherwise we chose the highest job number
	// + 1
	if(job_length == 0) {
		job.id = 1;
	} else {
		const ShellJobState* arg =
		    TVEC_GET_AT(ShellJobState, &(state->jobs_state.array), job_length - 1);
		ASSERT(arg != NULL);
		job.id = arg->id + 1;
	}

	TvecResult res = TVEC_PUSH(ShellJobState, &(state->jobs_state.array), job);

	if(res != TvecResultOk) { // NOLINT(readability-implicit-bool-conversion)
		RETURNS_VOID shell_global_exit(state, ExitCodeImplementationError);

		RETURNS_VOID shell_print_error("[INTERNAL] can't append to jobs array\n");

		return;
	}

	int fres = 0;
	if(sync.type == ShellCommandSynchronizationTypeProcess) {
		fres = fprintf(stdout, "[%zu] %d\n", job.id, sync.data.process.handle.child_pid);

	} else if(sync.type == ShellCommandSynchronizationTypeThread) {
		fres = fprintf(stdout, "[%zu] %zu\n", job.id, sync.data.thread.thread_id);
	} else {
	}

	if(fres < 0) {
		RETURNS_VOID shell_global_exit(state, ExitCodeIOError);

		RETURNS_VOID shell_print_error("[INTERNAL] output error\n");

		return;
	}
}

TVEC_DEFINE_AND_IMPLEMENT_VEC_TYPE(ShellCommandSynchronization)

typedef TVEC_TYPENAME(ShellCommandSynchronization) ShellCommandSynchronizationArray;

TVEC_DEFINE_AND_IMPLEMENT_VEC_TYPE(ShellPipeState)

typedef TVEC_TYPENAME(ShellPipeState) ShellPipeStateArray;

static void free_shell_pipe_states(ShellPipeStateArray* pipe_states) {

	TVEC_FREE(ShellPipeState, pipe_states);
}
static void free_shell_pipe_info(ShellPipeInfoArray* pipe_info_array) {

	// TODO(Totto): maybe we should use close and check if all fds are already closed

	TVEC_FREE(ShellPipeInfo, pipe_info_array);
}

static void shell_execute_pipeline(const ShellParseDataPipeline* pipeline, ShellState* state) {

	const size_t pipeline_length = TVEC_LENGTH(ShellCommand, pipeline->commands);

	if(pipeline_length == 0) {
		RETURNS_VOID shell_global_exit(state, ExitCodeImplementationError);

		RETURNS_VOID shell_print_error("[INTERNAL] Invalid empty pipeline\n");
		return;
	}

	// do this synchronously, as we don't need complicated stuff for just one
	// command
	if(pipeline_length == 1) {
		const ShellCommand* const command = TVEC_GET_AT(ShellCommand, &(pipeline->commands), 0);
		ASSERT(command != NULL);

		if(command->settings.is_background) {
			RETURNS_VOID shell_execute_command_as_background_job(state, command);
		} else {
			RETURNS_VOID shell_execute_command_synchronous(state, command);
		}

		return;
	}

	// setup pipeline and sync state
	// we need an array for pipes, and than a array to hold sync states (either
	// for the subshells thread of the builtin or subshell pids of the child
	// processes for executables)
	ShellPipeStateArray pipe_states = TVEC_EMPTY(ShellPipeState);
	ShellCommandSynchronizationArray synchronization = TVEC_EMPTY(ShellCommandSynchronization);

	{
		TvecResult res = TVEC_ALLOCATE_UNINITIALIZED(ShellPipeState, &pipe_states, pipeline_length);

		if(res != TvecResultOk) { // NOLINT(readability-implicit-bool-conversion)
			RETURNS_VOID shell_global_exit(state, ExitCodeImplementationError);

			RETURNS_VOID shell_print_error("OOM in executing pipeline\n");
			return;
		}
	}

	{
		TvecResult res2 = TVEC_ALLOCATE_UNINITIALIZED(ShellCommandSynchronization, &synchronization,
		                                              pipeline_length);

		if(res2 != TvecResultOk) { // NOLINT(readability-implicit-bool-conversion)
			RETURNS_VOID shell_global_exit(state, ExitCodeImplementationError);

			RETURNS_VOID shell_print_error("OOM in executing pipeline\n");
			return;
		}
	}

	ShellPipeInfoArray pipe_info_array = TVEC_EMPTY(ShellPipeInfo);

	{
		TvecResult res3 =
		    TVEC_ALLOCATE_UNINITIALIZED(ShellPipeInfo, &pipe_info_array, pipeline_length - 1);

		if(res3 != TvecResultOk) { // NOLINT(readability-implicit-bool-conversion)
			RETURNS_VOID shell_global_exit(state, ExitCodeImplementationError);

			RETURNS_VOID shell_print_error("OOM in executing pipeline\n");
			return;
		}
	}

	{ // fill the pipe array

		for(size_t i = 0; i < (pipeline_length - 1); ++i) {

			ShellPipeInfo* const pipe_info = TVEC_GET_AT_MUT(ShellPipeInfo, &pipe_info_array, i);
			ASSERT(pipe_info != NULL);

			*pipe_info = (ShellPipeInfo){ .fds = { 0, 0 } };
			int res = pipe(pipe_info->fds);

			if(res != 0) {
				RETURNS_VOID shell_global_exit(state, ExitCodeImplementationError);

				RETURNS_VOID shell_print_error("error in creating pipe\n");
				goto pipeline_cleanup;
			}
		}
	}

	{
		for(size_t i = 0; i < pipeline_length; ++i) {

			ShellPipeState* const pipe_state = TVEC_GET_AT_MUT(ShellPipeState, &pipe_states, i);
			ASSERT(pipe_state != NULL);
			ShellCommandSynchronization* const sync =
			    TVEC_GET_AT_MUT(ShellCommandSynchronization, &synchronization, i);
			ASSERT(sync != NULL);

			*sync =
			    (ShellCommandSynchronization){ .type = ShellCommandSynchronizationTypeError,
				                               .data = { .error = TSTR_LIT("Not filled yet") } };

			*pipe_state = (ShellPipeState){
				.array = &pipe_info_array,
				.settings = {
					.stdin = { .is_pipe = false, .idx = 0 },
					.stdout = { .is_pipe = false, .idx = 0 },
				},
				.close = { .start = 0, .end = 0 },
			};

			if(i > 0) {
				const size_t idx = i - 1;
				pipe_state->settings.stdin = (ShellMaybePipe){ .is_pipe = true, .idx = idx };
				pipe_state->close = (ShellPipeClose){ .start = idx, .end = idx + 1 };
			}

			if(i < (pipeline_length - 1)) {

				pipe_state->settings.stdout = (ShellMaybePipe){ .is_pipe = true, .idx = i };

				const size_t start = i == 0 ? i : pipe_state->close.start;

				pipe_state->close = (ShellPipeClose){ .start = start, .end = i + 1 };
			}
		}
	}

	for(size_t i = 0; i < TVEC_LENGTH(ShellCommand, pipeline->commands); ++i) {

		const ShellCommand* const command = TVEC_GET_AT(ShellCommand, &(pipeline->commands), i);
		ASSERT(command != NULL);
		ShellCommandSynchronization* const sync =
		    TVEC_GET_AT_MUT(ShellCommandSynchronization, &synchronization, i);
		ASSERT(sync != NULL);
		const ShellPipeState* const pipe_state = TVEC_GET_AT(ShellPipeState, &pipe_states, i);
		ASSERT(pipe_state != NULL);

		if(command->settings.is_background) {
			// commands in piplines can't be background jobs yet
			RETURNS_VOID shell_global_exit(state, ExitCodeImplementationError);

			RETURNS_VOID shell_print_error(
			    "[INTERNAL]: commands in piplines can't be background jobs yet\n");

			goto pipeline_cleanup;
		}

		// if we have a executable, launch it with the pipe settings and get the pid
		// for a builtin start a new thread and get the thread_id
		RETURNS_VOID shell_execute_command_in_subshell(state, command, pipe_state, sync);

		if(sync->type == ShellCommandSynchronizationTypeError) {
			RETURNS_VOID shell_global_exit(state, ExitCodeImplementationError);

			RETURNS_VOID shell_print_error("[INTERNAL]: " TSTR_FMT "\n",
			                               TSTR_FMT_ARGS(sync->data.error));
			tstr_free(&(sync->data.error));

			goto pipeline_cleanup;
		}
		//
	}

	// TODO(Totto): here we should implement pipefail, when we need to

	// wait for all steps of the pipeline to end

	for(size_t i = 0; i < TVEC_LENGTH(ShellCommand, pipeline->commands); ++i) {

		// just walk linearly through all of them we need to wait nevertheless for
		// all, so might as well do left to right in a pipline, no need for fancy
		// waitpid(WNOHANG) or condition variables for threads

		ShellCommandSynchronization* const sync =
		    TVEC_GET_AT_MUT(ShellCommandSynchronization, &synchronization, i);
		ASSERT(sync != NULL);
		const ShellPipeState* const pipe_state = TVEC_GET_AT(ShellPipeState, &pipe_states, i);
		ASSERT(pipe_state != NULL);

		switch(sync->type) {
			case ShellCommandSynchronizationTypeProcess: {

				int status = 0;
				int res = waitpid(sync->data.process.handle.child_pid, &status, 0);
				if(res < 0) {
					RETURNS_VOID shell_global_exit(state, ExitCodeImplementationError);

					RETURNS_VOID shell_print_error("error in waitpid\n");
					goto pipeline_cleanup;
				}

				if(WIFEXITED(status)) {
					uint8_t exit_status = (uint8_t)WEXITSTATUS(status);
					// this is only needed for pipefail
					UNUSED(exit_status);
				} else if(WIFSIGNALED(status)) {
					uint8_t signal_number = WTERMSIG(status);
					// this is only needed for pipefail
					UNUSED(signal_number);
				} else {
					RETURNS_VOID shell_global_exit(state, ExitCodeImplementationError);

					RETURNS_VOID shell_print_error("process not exited\n");
					goto pipeline_cleanup;
				}

				// handle pipes, these need to be cleaned up in this scenario only and
				// after the processes, as the fork duplicates the pipe fds and they need
				// to be closed in the forked and this process

				if(pipe_state->settings.stdin.is_pipe) {
					const ShellPipeInfo* const stdin_info = TVEC_GET_AT(
					    ShellPipeInfo, pipe_state->array, pipe_state->settings.stdin.idx);
					ASSERT(stdin_info != NULL);

					// close the unnecessary READ_END
					int cres = close(stdin_info->fds[PIPE_READ_END]);
					if(cres != 0) {
						RETURNS_VOID shell_global_exit(state, ExitCodeImplementationError);

						RETURNS_VOID shell_print_error("close error\n");
						goto pipeline_cleanup;
					}
				}

				if(pipe_state->settings.stdout.is_pipe) {
					const ShellPipeInfo* const stdout_info = TVEC_GET_AT(
					    ShellPipeInfo, pipe_state->array, pipe_state->settings.stdout.idx);
					ASSERT(stdout_info != NULL);

					// close the unnecessary WRITE_END
					int cres = close(stdout_info->fds[PIPE_WRITE_END]);
					if(cres != 0) {
						RETURNS_VOID shell_global_exit(state, ExitCodeImplementationError);

						RETURNS_VOID shell_print_error("close error\n");
						goto pipeline_cleanup;
					}
				}

				break;
			}
			case ShellCommandSynchronizationTypeThread: {

				void* return_value = NULL;
				int res = pthread_join(sync->data.thread.thread_id, &return_value);

				if(res != 0) {
					RETURNS_VOID shell_global_exit(state, ExitCodeImplementationError);

					RETURNS_VOID shell_print_error("process not exited\n");
					goto pipeline_cleanup;
				}

				RETURNS_VOID free_sub_shell_thread_state(sync->data.thread.state);
				break;
			}
			case ShellCommandSynchronizationTypeError: {
				RETURNS_VOID shell_global_exit(state, ExitCodeImplementationError);

				RETURNS_VOID shell_print_error("[INTERNAL]: " TSTR_FMT "\n",
				                               TSTR_FMT_ARGS(sync->data.error));

				tstr_free(&sync->data.error);
				goto pipeline_cleanup;
			}
			default: {
				RETURNS_VOID shell_global_exit(state, ExitCodeImplementationError);

				RETURNS_VOID shell_print_error("[INTERNAL] invalid shell command sync type\n");
				goto pipeline_cleanup;
			}
		}
	}

pipeline_cleanup:
	// cleanup pipes, if they are forked, we need to close them here, otherwise we
	// need to cleanup after the builtins

	// note: internal state are already cleared out in case of a successful traversal
	TVEC_FREE(ShellCommandSynchronization, &synchronization);
	free_shell_pipe_states(&pipe_states);
	free_shell_pipe_info(&pipe_info_array);
}

static void shell_repl_step(ShellState* state) {

	if(!shell_process_input_state(state)) {

		RETURNS_VOID shell_global_exit(state, ExitCodeGenericError);

		RETURNS_VOID shell_print_error("[INTERNAL] prompt print error\n");
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

		RETURNS_VOID shell_print_error(TSTR_FMT "\n", TSTR_FMT_ARGS(error));
		tstr_free(&error);
		return;
	}

	RTTIAnnotatedValue state_rtti = TRTTI_ANNOTATED_VALUE_GET(ShellState, state);
	ShellHandle shell_handle = SHELL_HANDLE_SYNCHRONOUS(state_rtti);

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

				int res = -1;
				if(command_not_found->reason == ShellExecutableErrorTypeGeneric ||
				   command_not_found->reason == ShellExecutableErrorTypeNotFound) {

					res = fprintf(stderr, TSTR_FMT ": command not found\n",
					              TSTR_FMT_ARGS(command_not_found->name));
				} else {
					res = fprintf(stderr, TSTR_FMT ": permission denied\n",
					              TSTR_FMT_ARGS(command_not_found->name));
				}

				if(res < 0) {
					RETURNS_VOID shell_global_exit(state, ExitCodeIOError);

					RETURNS_VOID shell_print_error("[INTERNAL] output error\n");

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

				RETURNS_VOID shell_print_error("[INTERNAL] output error\n");
				break;
			}
			set_command_exit_code(state, ExitCodeGenericError);
			break;
		}
		default: {
			RETURNS_VOID shell_global_exit(state, ExitCodeImplementationError);

			RETURNS_VOID shell_print_error("[INTERNAL] Invalid parse result\n");
			break;
		}
	}

#if SHELL_USE_EDIT_LIBRARY == 2
	if(data.end) {
		RETURNS_VOID shell_global_exit(state, state->command_state.exit_code);
	}
#endif

	RETURNS_VOID free_shell_input_ok_result(data);

	RETURNS_VOID shell_reap_background_jobs(state);
}

static void shell_populate_history(ShellState* state) {

	const char* const histfile_env = getenv("HISTFILE");

	if(histfile_env == NULL) {
		return;
	}

	tstr file = tstr_from(histfile_env);

	RTTIAnnotatedValue state_rtti = TRTTI_ANNOTATED_VALUE_GET(ShellState, state);

	tstr result =
	    read_history_to_file(&file, shell_virtual_fn_sync_read_history_file_impl_cb, state_rtti);

	if(!tstr_is_null(&result)) {
		tstr_free(&file);

		RETURNS_VOID shell_global_exit(state, ExitCodeGenericError);

		RETURNS_VOID shell_print_error("[INTERNAL] history file population failed\n");
		return;
	}

	state->global.history_state.last_append_idx = TVEC_LENGTH(tstr, state->global.history.array);
	state->global.history_state.histfile = file;
}

static void shell_populate_state(ShellState* state) {

	RETURNS_VOID shell_populate_history(state);

	{

		const char* const shell_global_debug_env = getenv("SHELL_GLOBAL_DEBUG");

		if(shell_global_debug_env == NULL) {
			state->settings.debug = false;
		} else {
			// TODO: parse the value, accept truthy values and set the result
			state->settings.debug = true;
		}
	}
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

	// TODO(Totto): setup custom libedit behaviour
	//  this includes: signal handling, tab completion, history

	ShellInputState input_state = { .el = el };
#elif SHELL_USE_EDIT_LIBRARY == 1
	UNUSED(program_name);
	int res = rl_initialize();
	if(res != 0) {
		return (ShellSetupResult){ .type = ShellSetupResultTypeError,
			                       .data = { .error = TSTR_LIT("readline init error") } };
	}

	using_history();

	// TODO(Totto): setup custom readline behaviour
	//  this includes: signal handling

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
		.command_state = { .exit_code = ExitCodeSuccess },
		.interactive = interactive,
		.input_state = input_state,
		.global = { .alive = true,
		            .result = ExitCodeSuccess,
		            .cwd = cwd,
		            .history = { .array = TVEC_EMPTY(tstr) ,
					} ,
					.history_state = {.last_append_idx = 0,.histfile = tstr_null() },
					.variables= { .map = TMAP_EMPTY(ShellVariableHashMap) },
			},
		.completion_state =  {
			.completion_trie = NULL,
			.storage={ .map = TMAP_EMPTY(ShellCompletionHashMap) },
		},
		.jobs_state = {.array = TVEC_EMPTY(ShellJobState)},
		.settings = { .debug = false }
	};

	RETURNS_VOID shell_populate_state(&state);

	return (ShellSetupResult){ .type = ShellSetupResultTypeOk, .data = { .ok = state } };
}

#if SHELL_USE_EDIT_LIBRARY == 0
static bool shell_add_to_input_history(const tstr* command) {
	// TODO: implement
	UNUSED(command);
	return true;
}
static void free_input_history(ShellState* state) {
	// TODO: implement
	UNUSED(state);
}
#elif SHELL_USE_EDIT_LIBRARY == 1
static bool shell_add_to_input_history(const tstr* command) {
	RETURNS_VOID add_history(tstr_cstr(command));
	return true;
}
static void free_input_history(ShellState* state) {
	UNUSED(state);
	clear_history();
}
#else
static bool shell_add_to_input_history(const tstr* command) {
	// TODO: implement
	UNUSED(command);
	return true;
}
static void free_input_history(ShellState* state) {
	// TODO: implement
	UNUSED(state);
}
#endif

static void free_completion_storage(ShellCompleteStorage* storage) {
	TMAP_TYPENAME_ITER(ShellCompletionHashMap)
	iter = TMAP_ITER_INIT(ShellCompletionHashMap, &(storage->map));

	TMAP_TYPENAME_ENTRY(ShellCompletionHashMap) value;

	while(TMAP_ITER_NEXT(ShellCompletionHashMap, &iter, &value)) {
		tstr_free(&value.key);
		free_shell_completion(&value.value);
	}

	TMAP_FREE(ShellCompletionHashMap, &(storage->map));
}

static void free_shell_completion_state(ShellCompletionState* completion_state) {

	if(completion_state->completion_trie != NULL) {
		free_trie(completion_state->completion_trie);
	}

	free_completion_storage(&(completion_state->storage));
}

static void free_shell_state(ShellState* state) {

	free_shell_instance_state(&(state->global));

#if SHELL_USE_EDIT_LIBRARY == 0
	el_end(state->input_state.el);
	shell_cleanup_old_prompt(state);
#endif

	free_shell_completion_state(&(state->completion_state));

	free_input_history(state);

	free_shell_jobs_state(&(state->jobs_state));
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

	RETURNS_VOID shell_print_error("[INTERNAL] prompt print error\n");
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
	// TODO(Totto): set editor, for standard shortcuts
	// el_set(EL_EDITOR, "emacs" "vi")
	return true;
#elif SHELL_USE_EDIT_LIBRARY == 1

	return shell_set_prompt_to_input_state(state);
#else
	UNUSED(state);
	return true;
#endif
}

typedef enum : uint8_t {
	// first FF bytes are for flag type
	TrieFlagTypeNone = 0x00,
	TrieFlagTypeBuiltin = 0x01,
	TrieFlagTypeExecutable = 0x02,
	TrieFlagTypeFile = 0x04,
	//
	TrieFlagTypeMask = 0xFF,
} TrieFlagType;

typedef enum : TrieEntryFlag {
	// first FF bytes are for flag type
	TrieFlagFileFlagValUnknown = 0x0100,
	TrieFlagFileFlagValFile = 0x0200,
	TrieFlagFileFlagValDir = 0x0400,
} TrieFlagFile;

NODISCARD MAYBE_UNUSED static Trie* shell_get_completion_trie_for_dir(const tstr* path,
                                                                      tstr_view dir_prefix) {

	Trie* trie = initialize_trie();

	if(trie == NULL) {
		return NULL;
	}

	DIR* dir = opendir(tstr_cstr(path));

	if(dir == NULL) {
		free_trie(trie);
		return NULL;
	}

	while(true) {
		errno = 0;
		struct dirent* ent = readdir(dir);
		if(ent == NULL) {
			if(errno != 0) {
				// and error occurred
				free_trie(trie);
				IGNORE_RESULT closedir(dir);
				return NULL;
			}

			break;
		}

		tstr_view name = tstr_view_from(ent->d_name);

		if(tstr_view_eq(name, ".") || tstr_view_eq(name, "..")) {
			continue;
		}

		const TrieFlagFile file_type =
		    TrieFlagTypeFile | (ent->d_type == DT_UNKNOWN ? TrieFlagFileFlagValUnknown
		                        : ent->d_type == DT_DIR   ? TrieFlagFileFlagValDir
		                                                  : TrieFlagFileFlagValFile);

		tstr value = tstr_from_view(dir_prefix);

		{
			const TStrResult res = tstr_cat_len(&value, name.data, name.len);

			if(res != TStrResultOk) {
				free_trie(trie);
				IGNORE_RESULT closedir(dir);
				return NULL;
			}

			if(ent->d_type == DT_DIR) {
				const TStrResult res2 = tstr_cat_len(&value, "/", 1);
				if(res2 != TStrResultOk) {
					free_trie(trie);
					IGNORE_RESULT closedir(dir);
					return NULL;
				}
			}
		}

		bool ok = add_to_trie(trie, value, file_type);
		if(!ok) {
			free_trie(trie);
			IGNORE_RESULT closedir(dir);
			return NULL;
		}
	}

	IGNORE_RESULT closedir(dir);

	return trie;
}

NODISCARD static bool shell_add_executables_in_dir_to_completion(Trie* trie, tstr_view directory) {

	tstr dir_str = tstr_from_view(directory);

	DIR* dir = opendir(tstr_cstr(&dir_str));
	tstr_free(&dir_str);

	if(dir == NULL) {
		return true;
	}
	while(true) {
		errno = 0;
		struct dirent* ent = readdir(dir);
		if(ent == NULL) {
			if(errno != 0) {
				// and error occurred
				return false;
			}

			break;
		}

		tstr_view name = tstr_view_from(ent->d_name);

		if(tstr_view_eq(name, ".") || tstr_view_eq(name, "..")) {
			continue;
		}

		ShellExecutableResult executable_result =
		    path_dir_get_executable_from_parts(directory, name);
		if(!executable_result.is_ok) {
			continue;
		}

		tstr_free(&executable_result.data.ok);

		bool ok = add_to_trie(trie, tstr_from_view(name), TrieFlagTypeExecutable);
		if(!ok) {
			return false;
		}
	}

	IGNORE_RESULT closedir(dir);

	return true;
}

NODISCARD static bool shell_add_executables_in_path_to_completion(Trie* trie) {
	const char* const path_env = getenv("PATH");

	if(path_env == NULL) {
		return true;
	}

	PathDirectoriesResult path_res = get_path_directories(tstr_view_from(path_env));

	if(!path_res.is_ok) {
		// Note: the error is ignored here
		tstr_free(&path_res.data.error);
		return true;
	}

	PathDirectories directories = path_res.data.directories;

	for(size_t i = 0; i < TVEC_LENGTH(PathDirectory, directories); ++i) {

		const PathDirectory* const arg = TVEC_GET_AT(PathDirectory, &directories, i);
		ASSERT(arg != NULL);

		bool ok = shell_add_executables_in_dir_to_completion(trie, arg->path);

		if(!ok) {
			free_path_directories(&directories);
			return false;
		}
	}

	free_path_directories(&directories);
	return true;
}

NODISCARD MAYBE_UNUSED static bool shell_populate_completion_state(ShellState* state) {

	Trie* trie = initialize_trie();

	if(trie == NULL) {
		return false;
	}

	// NOTE: we never update this list, as we don#t need this, in the ideal case, we store functions
	// about the PATH string, reindex every directory, once it gets changed and also store the mtime
	// for all directories, we get the executables from, so that when one of them changes, we just
	// reindex everything

	// add builtins
	for(size_t i = 0; i < BUILTIN_FUNCTION_COUNT; ++i) {

		BuiltinCommand builtin_command = g_builtin_functions[i];
		bool ok =
		    add_to_trie(trie, tstr_from_static_tstr(builtin_command.name), TrieFlagTypeBuiltin);
		if(!ok) {
			return false;
		}
	}

	// add paths
	bool ok = shell_add_executables_in_path_to_completion(trie);
	if(!ok) {
		return false;
	}

	state->completion_state.completion_trie = trie;
	return true;
}

#if SHELL_USE_EDIT_LIBRARY == 0
NODISCARD static bool shell_setup_completion(ShellState* state) {

	// TODO: implement
	UNUSED(state);
	return true;
}
#elif SHELL_USE_EDIT_LIBRARY == 1

typedef struct {
	char** raw;
} RLCompletionArray;

typedef enum : uint8_t {
	RLCompletionResultTypeOk = 0,
	RLCompletionResultTypeEmpty,
	RLCompletionResultTypeError,
} RLCompletionResultType;

typedef struct {
	RLCompletionArray array;
	bool suppress_append;
} RLCompletionOk;

typedef struct {
	RLCompletionResultType type;
	union {
		RLCompletionOk ok;
		tstr error;
	} data;
} RLCompletionResult;

NODISCARD static RLCompletionResult
completion_result_array_to_rl_completion_array(TrieResultEntryArray* array,
                                               tstr_view current_prefix) {

	const size_t array_length = TVEC_LENGTH(TrieResultEntry, *array);

	if(array_length == 0) {
		TVEC_FREE(TrieResultEntry, array);

		if(g_is_docker) {
			fprintf(stderr, "array_length is 0\n");
		}

		return (RLCompletionResult){ .type = RLCompletionResultTypeEmpty, .data = {} };
	}

	bool suppress_append = false;

	if(array_length == 1) {
		const TrieResultEntry* const arg0 = TVEC_GET_AT(TrieResultEntry, array, 0);
		ASSERT(arg0 != NULL);

		uint8_t flags = (uint8_t)(arg0->flag & TrieFlagTypeMask);

		if(flags == TrieFlagTypeFile) {
			if((arg0->flag & TrieFlagFileFlagValDir) != 0) {
				suppress_append = true;
			}
		}
	}

	// NOTE: this array is of size (length + 2) of char*
	// the first entry is the common prefix (nonull!)
	// the end is marked by NULL
	char** raw_array = malloc((array_length + 2) * sizeof(char*));

	if(raw_array == NULL) {
		return (RLCompletionResult){ .type = RLCompletionResultTypeError,
			                         .data = { .error = TSTR_LIT("OOM in internal allocation") } };
	}

	bool correct_script = false;

	tstr common_prefix = find_common_prefix(array);

	{

		if(tstr_is_null(&common_prefix)) {
			common_prefix = tstr_from_view(current_prefix);
		} else if(tstr_len(&common_prefix) < current_prefix.len) {
			correct_script = true;

			// if the completion script returns some "wrong" data, correct it
			// wrong means e.g. i specify "re" and it returns "read", "return" and "root", so the
			// calculated prefix is smaller then the result

			tstr_free(&common_prefix);
			common_prefix = tstr_from_view(current_prefix);
		}

		raw_array[0] = strdup(tstr_cstr(&common_prefix));

		if(!correct_script) {
			tstr_free(&common_prefix);
		}
	}

	if(correct_script) {
		size_t current_idx = 0;
		// we have some invalid entires, filter them out
		for(size_t i = 0; i < array_length; ++i) {

			const TrieResultEntry* const arg = TVEC_GET_AT(TrieResultEntry, array, i);
			ASSERT(arg != NULL);

			if(!tstr_view_starts_with(arg->name, tstr_cstr(&common_prefix))) {
				continue;
			}
			tstr name = tstr_from_view(arg->name);

			raw_array[current_idx + 1] = strdup(tstr_cstr(&name));
			tstr_free(&name);

			++current_idx;
		}

		raw_array[current_idx + 1] = NULL;
		tstr_free(&common_prefix);
	} else {

		raw_array[array_length + 1] = NULL;

		for(size_t i = 0; i < array_length; ++i) {

			const TrieResultEntry* const arg = TVEC_GET_AT(TrieResultEntry, array, i);
			ASSERT(arg != NULL);

			tstr name = tstr_from_view(arg->name);

			raw_array[i + 1] = strdup(tstr_cstr(&name));
			tstr_free(&name);
		}
	}

	TVEC_FREE(TrieResultEntry, array);

	return (RLCompletionResult){ .type = RLCompletionResultTypeOk,
		                         .data = { .ok = { .suppress_append = suppress_append,
		                                           .array = { .raw = raw_array } } } };
}

typedef struct {
	tstr abs_dir;
	tstr_view dir_prefix;
} FileNameParentResolveResultOk;

typedef struct {
	bool is_ok;
	union {
		FileNameParentResolveResultOk ok;
		tstr error;
	} data;
} FileNameParentResolveResult;

NODISCARD static FileNameParentResolveResult filename_resolve_parent(ShellState* state,
                                                                     tstr_view prefix) {
	tstr_view cwd = tstr_as_view(&state->global.cwd);

	if(cwd.len == 0) {
		return (FileNameParentResolveResult){ .is_ok = false,
			                                  .data = { .error = TSTR_LIT("cwd get res") } };
	}

	tstr resolved_path = tstr_null();

	// path_resolve_to_absolute doesn't handle empty paths, we have to do it manually, this is a
	// choice, as other users of this API expect that
	if(prefix.len == 0) {
		resolved_path = path_resolve_to_absolute(cwd, (tstr_view){ .data = "a", .len = 1 });
	} else {
		resolved_path = path_resolve_to_absolute(cwd, prefix);
	}
	if(tstr_is_null(&resolved_path)) {
		return (FileNameParentResolveResult){ .is_ok = false,
			                                  .data = { .error = TSTR_LIT("path resolve res") } };
	}

	if(!path_is_absolute(tstr_as_view(&resolved_path))) {
		tstr_free(&resolved_path);

		return (FileNameParentResolveResult){
			.is_ok = false, .data = { .error = TSTR_LIT("resolved path is not absolute") }
		};
	}

	const bool is_dir = tstr_view_ends_with(tstr_as_view(&resolved_path), "/");

	tstr path = path_normalize(tstr_as_view(&resolved_path));

	tstr_free(&resolved_path);

	if(tstr_is_null(&path)) {
		return (FileNameParentResolveResult){ .is_ok = false,
			                                  .data = { .error = TSTR_LIT("path normalize res") } };
	}

	if(!path_is_absolute(tstr_as_view(&path))) {
		tstr_free(&path);

		return (FileNameParentResolveResult){
			.is_ok = false, .data = { .error = TSTR_LIT("normalized path is not absolute") }
		};
	}

	// now get the parent path
	if(tstr_len(&path) == 0) {
		tstr_free(&path);

		return (FileNameParentResolveResult){
			.is_ok = false, .data = { .error = TSTR_LIT("can't get the parent path of this") }
		};
	}

	if(is_dir) {
		return (FileNameParentResolveResult){
			.is_ok = true, .data = { .ok = { .abs_dir = path, .dir_prefix = prefix } }
		};
	}

	tstr_view parent_view = tstr_as_view(&path);

	while(true) {
		const bool end_with_slash = tstr_view_ends_with(parent_view, "/");
		if(end_with_slash) {
			if(parent_view.len > 1) {
				parent_view = tstr_sub(parent_view, 0, parent_view.len - 1);
			}
			break;
		}

		parent_view = tstr_sub(parent_view, 0, parent_view.len - 1);
	}

	tstr parent_path = tstr_from_view(parent_view);
	tstr_free(&path);

	tstr_view dir_prefix = prefix;

	while(true) {
		if(dir_prefix.len == 0) {
			break;
		}

		bool end_with_slash = tstr_view_ends_with(dir_prefix, "/");
		if(end_with_slash) {
			break;
		}

		dir_prefix = tstr_sub(dir_prefix, 0, dir_prefix.len - 1);
	}

	return (FileNameParentResolveResult){
		.is_ok = true, .data = { .ok = { .abs_dir = parent_path, .dir_prefix = dir_prefix } }
	};
}

NODISCARD static RLCompletionResult rl_complete_filename_impl(ShellState* state, tstr_view prefix) {

	FileNameParentResolveResult parent_res = filename_resolve_parent(state, prefix);

	if(!parent_res.is_ok) {
		return (RLCompletionResult){ .type = RLCompletionResultTypeError,
			                         .data = { .error = parent_res.data.error } };
	}

	FileNameParentResolveResultOk ok_res = parent_res.data.ok;

	Trie* trie = shell_get_completion_trie_for_dir(&(ok_res.abs_dir), ok_res.dir_prefix);

	tstr_free(&ok_res.abs_dir);

	if(trie == NULL) {
		return (RLCompletionResult){ .type = RLCompletionResultTypeError,
			                         .data = { .error = TSTR_LIT("Can't make trie") } };
	}

	TrieSearchResult res = search_in_trie(trie, prefix);

	if(!res.is_ok) {

		return (RLCompletionResult){ .type = RLCompletionResultTypeError,
			                         .data = { .error = res.data.error } };
	}

	TrieResultEntryArray array = res.data.ok;

	RLCompletionResult completion_res =
	    completion_result_array_to_rl_completion_array(&array, prefix);
	free_trie(trie);

	return completion_res;
}

typedef struct {
	ShellArgument arg0;
	ShellArguments args;
	const ShellCompletion* completion;
} ShellResolvedCompletionLine;

typedef struct {
	bool is_ok;
	union {
		ShellResolvedCompletionLine resolved;
	} data;
} ShellCompletionLineResult;

NODISCARD static ShellCompletionLineResult shell_resolve_completion_line_impl(ShellState* state,
                                                                              tstr_view prefix) {

	// first get the command, if there is one, for that use the parser, and if it returns a command,
	// use the last one
	// TODO(Totto): do we select the correct command here based on the result? doe rl take the
	// cursor in consideration or not?

	RTTIAnnotatedValue state_rtti = TRTTI_ANNOTATED_VALUE_GET(ShellState, state);
	ShellHandle shell_handle = SHELL_HANDLE_SYNCHRONOUS(state_rtti);
	ShellParseDataResult parse = shell_parse_data(prefix, shell_handle);

	if(g_is_docker) {
		fprintf(stderr, "TRYING TO DO DOCKER: completion check: '" TSTR_FMT "' parse.type: %d\t",
		        TSV_FMT_ARGS(prefix), parse.type);
	}

	switch(parse.type) {
		case ShellParseDataResultTypePipeline: {
			ShellParseDataPipeline pipeline = parse.data.pipeline;

			ShellCompletionLineResult result = { .is_ok = false, .data = {} };

			// duplicate the last entry
			const size_t pipeline_length = TVEC_LENGTH(ShellCommand, pipeline.commands);

			if(g_is_docker) {
				fprintf(stderr, "TRYING TO DO DOCKER: pipeline_length %zu\t", pipeline_length);
			}

			if(pipeline_length == 0) {

				result = (ShellCompletionLineResult){ .is_ok = false, .data = {} };
				goto return_result;
			}

			ShellCommand* const command =
			    TVEC_GET_AT_MUT(ShellCommand, &(pipeline.commands), (pipeline_length - 1));
			ASSERT(command != NULL);

			const ShellArgument arg0 = command->arg0;

			const ShellCompletion* completion =
			    shell_get_completion_impl(&(state->completion_state), &(arg0.parsed));

			if(g_is_docker) {
				fprintf(stderr, "TRYING TO DO DOCKER: completion ptr %p\t",
				        (const void*)completion);
			}

			if(completion == NULL) {
				result = (ShellCompletionLineResult){ .is_ok = false, .data = {} };
				goto return_result;
			}

			result = (ShellCompletionLineResult){ .is_ok = true,
				                                  .data = { .resolved = {
				                                                .arg0 = arg0,
				                                                .args = command->args,
				                                                .completion = completion,
				                                            } } };

			// we move the arg0 and args out of the command, before it gets free, so we don't need
			// to copy it
			command->arg0 = (ShellArgument){ .parsed = tstr_null(), .original = TSTR_EMPTY_VIEW };

			command->args = TVEC_EMPTY(ShellArgument);

		return_result:
			free_shell_pipeline(&pipeline);

			return result;
		}
		case ShellParseDataResultTypeCommandsNotFound: {
			free_shell_commands_not_found(&parse.data.not_found);

			return (ShellCompletionLineResult){ .is_ok = false, .data = {} };
		}
		case ShellParseDataResultTypeEmpty: {
			return (ShellCompletionLineResult){ .is_ok = false, .data = {} };
		}
		case ShellParseDataResultTypeError: {

			tstr_free(&parse.data.error);
			return (ShellCompletionLineResult){ .is_ok = false, .data = {} };
		}
		default: {
			RETURNS_VOID shell_global_exit(state, ExitCodeImplementationError);

			RETURNS_VOID shell_print_error("[INTERNAL] Invalid parse result\n");
			return (ShellCompletionLineResult){ .is_ok = false, .data = {} };
		}
	}
}

static void free_resolved_completion_line(ShellResolvedCompletionLine* resolved) {
	free_shell_argument(&(resolved->arg0));
	free_shell_arguments(&(resolved->args));
}

NODISCARD static ShellArgument shell_dup_shell_argument(const ShellArgument* arg) {
	ShellArgument result = { .parsed = tstr_dup(&(arg->parsed)), .original = TSTR_EMPTY_VIEW };

	result.original = tstr_as_view(&result.parsed);

	return result;
}

NODISCARD static tstr rl_get_script_invoke_name(const tstr* path) {

	tstr_view whole = tstr_as_view(path);

	if(whole.len == 0) {
		return tstr_null();
	}

	size_t current_offset = whole.len;

	while(true) {

		if(current_offset == 0) {
			// the entire script is the name
			break;
		}

		const char character = whole.data[current_offset - 1];

		if(character == '/') {
			// we have the name, set whole to the name and break
			whole = tstr_sub_until_end(whole, current_offset);
			break;
		}

		current_offset--;
	}

	return tstr_from_view(whole);
}

NODISCARD static RLCompletionResult
rl_complete_completion_impl(ShellResolvedCompletionLine completion, tstr_view prefix,
                            tstr_view all_text, size_t end) {

	const tstr* path = &(completion.completion->script);
	tstr invoke_name = rl_get_script_invoke_name(path);

	ShellArguments arguments = TVEC_EMPTY(ShellArgument);

	{

		ShellArgument argv1 = shell_dup_shell_argument(&(completion.arg0));

		TvecResult res = TVEC_PUSH(ShellArgument, &arguments, argv1);

		if(res != TvecResultOk) { /* NOLINT(readability-implicit-bool-conversion)*/
			return (RLCompletionResult){ .type = RLCompletionResultTypeError,
				                         .data = {
				                             .error = TSTR_LIT("OOM error in internal parsing") } };
		}

		ShellArgument argv2 = { .parsed = tstr_from_view(prefix), .original = prefix };

		res = TVEC_PUSH(ShellArgument, &arguments, argv2);

		if(res != TvecResultOk) { /* NOLINT(readability-implicit-bool-conversion)*/
			return (RLCompletionResult){ .type = RLCompletionResultTypeError,
				                         .data = {
				                             .error = TSTR_LIT("OOM error in internal parsing") } };
		}

		// NOTE: the 3rd argument is the previous word, if there is one
		ShellArgument argv3 = { .parsed = tstr_from(""), .original = TSTR_TSV("") };

		{
			const size_t args_length = TVEC_LENGTH(ShellArgument, completion.args);

			// we use the last argument from the args, or arg0, if there is none
			if(args_length > 0) {
				const ShellArgument* const arg_last =
				    TVEC_GET_AT(ShellArgument, &(completion.args), (args_length - 1));
				ASSERT(arg_last != NULL);
				argv3 = shell_dup_shell_argument(arg_last);
			} else if(!tstr_is_null(&(completion.arg0.parsed))) {
				argv3 = shell_dup_shell_argument(&completion.arg0);
			}
		}

		res = TVEC_PUSH(ShellArgument, &arguments, argv3);

		if(res != TvecResultOk) { /* NOLINT(readability-implicit-bool-conversion)*/
			return (RLCompletionResult){ .type = RLCompletionResultTypeError,
				                         .data = {
				                             .error = TSTR_LIT("OOM error in internal parsing") } };
		}
	}

	ShellEnvs envs = TVEC_EMPTY(ShellEnv);

	{

		ShellEnv env1 = { .name = TSTR_LIT("COMP_LINE"), .value = tstr_from_view(all_text) };

		TvecResult res = TVEC_PUSH(ShellEnv, &envs, env1);

		if(res != TvecResultOk) { /* NOLINT(readability-implicit-bool-conversion)*/
			return (RLCompletionResult){ .type = RLCompletionResultTypeError,
				                         .data = {
				                             .error = TSTR_LIT("OOM error in internal parsing") } };
		}

		ShellEnv env2 = { .name = TSTR_LIT("COMP_POINT"), .value = tstr_from("") };

		TStrResult str_res = tstr_fmt(&env2.value, "%zu", end);

		if(str_res != TStrResultOk) { /* NOLINT(readability-implicit-bool-conversion)*/
			return (RLCompletionResult){ .type = RLCompletionResultTypeError,
				                         .data = {
				                             .error = TSTR_LIT("OOM error in internal parsing") } };
		}
		res = TVEC_PUSH(ShellEnv, &envs, env2);

		if(res != TvecResultOk) { /* NOLINT(readability-implicit-bool-conversion)*/
			return (RLCompletionResult){ .type = RLCompletionResultTypeError,
				                         .data = {
				                             .error = TSTR_LIT("OOM error in internal parsing") } };
		}
	}

	ShellExecuteSyncCaptureResult exec_result =
	    invoke_executable_synchronous_capture(path, &invoke_name, &arguments, &envs);

	// TODO: remove, after we debugged this
	bool is_docker = false;

	// TODO: this tests some flaky codecarfter teets, is it my fault or the one from the script
	if(tstr_eq_cstr(&completion.arg0.parsed, "docker") || g_is_docker) {
		fprintf(stderr, "DETECTED DOCKER: '" TSTR_FMT "'\n'\n" TSTR_FMT "'\n",
		        TSTR_FMT_ARGS(completion.arg0.parsed), TSV_FMT_ARGS(all_text));
		is_docker = true;
	}

	free_resolved_completion_line(&completion);
	free_shell_arguments(&arguments);
	free_shell_envs(&envs);
	tstr_free(&invoke_name);

	if(exec_result.type != ShellExecuteSyncCaptureResultTypeOk) {

		if(exec_result.type == ShellExecuteSyncCaptureResultTypeError) {
			return (RLCompletionResult){ .type = RLCompletionResultTypeError,
				                         .data = { .error = exec_result.data.error } };
		} else if(exec_result.type == ShellExecuteSyncCaptureResultTypeCrash) {

			tstr error = tstr_from("");

			{

				ShellExecuteSyncCaptureResultCrash crash = exec_result.data.crash;

				if(crash.exit.normal_exit) {
					TStrResult str_res = tstr_fmt(&error, "The process exited with exit code %u",
					                              crash.exit.data.exit);
					if(str_res != TStrResultOk) { /* NOLINT(readability-implicit-bool-conversion)*/
						error = TSTR_LIT("The process exited with an unknown exit code");
					}
				} else {
					TStrResult str_res =
					    tstr_fmt(&error, "The process exited with signal <%u>: %s",
					             crash.exit.data.signal, strsignal(crash.exit.data.signal));
					if(str_res != TStrResultOk) { /* NOLINT(readability-implicit-bool-conversion)*/
						error = TSTR_LIT("The process exited with signal: <unknown signal>");
					}
				}

				TStrResult str_res = tstr_fmt(&error, "\n\nstderr:\n" TSTR_FMT "\n",
				                              TSTR_FMT_ARGS(crash.streams.stderr));
				if(str_res != TStrResultOk) { /* NOLINT(readability-implicit-bool-conversion)*/
					                          // just ignore here
				}

				tstr_free(&crash.streams.stdout);
				tstr_free(&crash.streams.stderr);
			}

			return (RLCompletionResult){ .type = RLCompletionResultTypeError,
				                         .data = { .error = error } };

		} else {
			return (RLCompletionResult){ .type = RLCompletionResultTypeError,
				                         .data = { .error = TSTR_LIT("Invalid execute result") } };
		}
	}

	ShellExecuteSyncCaptureResultOk ok = exec_result.data.ok;

	TrieResultEntryArray array = TVEC_EMPTY(TrieResultEntry);

	tstr_view stdout_data = tstr_as_view(&(ok.streams.stdout));

	size_t offset = 0;

	while(stdout_data.len != 0) {

		tstr_view data_to_add = TSTR_EMPTY_VIEW;

		if(offset >= stdout_data.len) {
			// we are at the end, add the entire thing
			data_to_add = stdout_data;
			stdout_data = TSTR_EMPTY_VIEW;
		} else {
			// check if we are at a newline
			const char character = stdout_data.data[offset];

			if(character == '\n') {
				// if we are handle this data

				if(offset == 0) {
					// ignore empty lines
					offset = 0;
					stdout_data = tstr_sub_until_end(stdout_data, 1);
					continue;
				}

				data_to_add = (tstr_view){ .data = stdout_data.data, .len = offset };
				stdout_data = tstr_sub_until_end(stdout_data, offset);
				offset = 0;
			} else {
				offset++;
				continue;
			}
		}

		TrieResultEntry entry = { .name = data_to_add, .flag = 0 };

		TvecResult res = TVEC_PUSH(TrieResultEntry, &array, entry);

		if(res != TvecResultOk) { // NOLINT(readability-implicit-bool-conversion)
			return (
			    RLCompletionResult){ .type = RLCompletionResultTypeError,
				                     .data = { .error = TSTR_LIT("OOM in argument allocation") } };
		}
	}

	if(is_docker) {
		fprintf(stderr, "DETECTED DOCKER: \n" TSTR_FMT "\n", TSV_FMT_ARGS(all_text));

		fprintf(stderr, "length: %zu\n", TVEC_LENGTH(TrieResultEntry, array));

		for(size_t i = 0; i < TVEC_LENGTH(TrieResultEntry, array); ++i) {

			const TrieResultEntry* const arg = TVEC_GET_AT(TrieResultEntry, &array, i);
			ASSERT(arg != NULL);

			fprintf(stderr, "arg: " TSTR_FMT "\n", TSV_FMT_ARGS(arg->name));
		}
		fprintf(stderr, "END DOCKER: \n");
	}

	RLCompletionResult result = completion_result_array_to_rl_completion_array(&array, prefix);
	tstr_free(&(ok.streams.stdout));
	tstr_free(&(ok.streams.stderr));

	return result;
}

NODISCARD static RLCompletionResult
shell_completion_function_rl_impl(ShellState* state, const char* text, int start, int end) {

	int len = end - start;
	if(len < 0) {
		return (RLCompletionResult){ .type = RLCompletionResultTypeError,
			                         .data = { .error = TSTR_LIT("invalid arguments") } };
	}

	tstr_view string = (tstr_view){ .data = text, .len = (size_t)len };

	if(start == 0) {

		// TODO: if the command starts with "~" "/" or ".", try to complete filenames and not
		// executables in PATH

		if(g_is_docker) {
			fprintf(stderr, "TRYING TO DO DOCKER: start == 0, so we try to complete docker and not "
			                "the argument\t");
		}

		// if start == 0, we have the first string, we complete commands on that

		const Trie* trie = state->completion_state.completion_trie;

		if(trie == NULL) {
			return (RLCompletionResult){ .type = RLCompletionResultTypeError,
				                         .data = { .error = TSTR_LIT("Trie empty") } };
		}

		TrieSearchResult res = search_in_trie(trie, string);

		if(!res.is_ok) {
			return (RLCompletionResult){ .type = RLCompletionResultTypeError,
				                         .data = { .error = res.data.error } };
		}

		TrieResultEntryArray array = res.data.ok;

		return completion_result_array_to_rl_completion_array(&array, string);
	}

	tstr_view before = (tstr_view){ .data = rl_line_buffer, .len = (size_t)(start - 1) };
	tstr_view everything = (tstr_view){ .data = rl_line_buffer, .len = (size_t)end };

	// we are in argument n of a command, we figure out which command, and if that command
	// has a completer script, get the completion from that, otherwise we just complete
	// filenames
	ShellCompletionLineResult completion = shell_resolve_completion_line_impl(state, before);

	if(g_is_docker) {
		fprintf(stderr, "TRYING TO DO DOCKER: completion is possible: %d\t", completion.is_ok);
	}

	if(completion.is_ok) {

		return rl_complete_completion_impl(completion.data.resolved, string, everything,
		                                   (size_t)end);
	}

	return rl_complete_filename_impl(state, string);
}

static ShellState* g_rl_internal_completion_state = NULL;

NODISCARD static char** shell_completion_function_rl(const char* text, int start, int end) {

	rl_attempted_completion_over = 1;

	bool is_docker = false;

	if(end >= 6 && rl_line_buffer[0] == 'd' && rl_line_buffer[1] == 'o' &&
	   rl_line_buffer[2] == 'c' && rl_line_buffer[3] == 'k' && rl_line_buffer[4] == 'e' &&
	   rl_line_buffer[5] == 'r') {
		if((end >= 9 && (rl_line_buffer[7] == 'p' && rl_line_buffer[8] == 's')) ||
		   (end >= 10 &&
		    (rl_line_buffer[7] == 'x' && rl_line_buffer[8] == 'y' && rl_line_buffer[9] == 'z'))) {

		} else {
			// TODO
			is_docker = true;
		}
	}

	if(is_docker) {
		g_is_docker = true;
		// fprintf(stderr, "TRYING TO DO DOCKER\t");
	}

	RLCompletionResult array =
	    shell_completion_function_rl_impl(g_rl_internal_completion_state, text, start, end);

	/* if(is_docker) {
	    fprintf(stderr, "array.type: %d %s\t", array.type,
	            array.type == RLCompletionResultTypeError   ? "error"
	            : array.type == RLCompletionResultTypeEmpty ? "empty"
	            : array.type == RLCompletionResultTypeOk    ? "ok"
	                                                        : "<ERRORROR>");
	} */

	if(array.type == RLCompletionResultTypeError) {
		shell_global_debug(g_rl_internal_completion_state,
		                   "[DEBUG]: error in the completer process: " TSTR_FMT "\n",
		                   TSTR_FMT_ARGS(array.data.error));
		tstr_free(&(array.data.error));

		rl_completion_suppress_append = 1;

		return NULL;
	} else if(array.type == RLCompletionResultTypeEmpty) {

		rl_completion_suppress_append = 1;

		return NULL;
	} else if(array.type == RLCompletionResultTypeOk) {

		RLCompletionOk ok = array.data.ok;

		if(is_docker) {

			size_t len = 0;
			char** ptr = ok.array.raw + 1;
			while(*ptr != NULL) {
				++len;
				ptr++;
			}

			// TODO: after some debugging, it seems that we get the files in teh cwd instead of the
			// completer here? why, debug further, we might have chosen to use filenames, as the
			// script is not executable or something? no idea why we chose the filenames instead of
			// the completer script??

			fprintf(stderr, "TRYING TO DO DOCKER: array length = %zu\t\n", len);

			fprintf(stderr, "TRYING TO DO DOCKER: array entry0 = %s\t\n", *(ok.array.raw + 1));
		}

		if(ok.suppress_append) {
			rl_completion_suppress_append = 1;
		} else {
			rl_completion_suppress_append = 0;
		}

		return ok.array.raw;
	} else {
		shell_global_debug(
		    g_rl_internal_completion_state,
		    "[DEBUG]: INTERNAL error in the completer code: invalid result type: %u\n", array.type);
		rl_completion_suppress_append = 1;

		return NULL;
	}
}

NODISCARD static bool shell_setup_completion(ShellState* state) {

	g_rl_internal_completion_state = state;

	rl_attempted_completion_function = shell_completion_function_rl;

	return shell_populate_completion_state(state);
}
#else
NODISCARD static bool shell_setup_completion(ShellState* state) {

	// TODO: implement
	UNUSED(state);
	return true;
}
#endif

ExitCode shell_execute_repl(const size_t argc, char* const* const argv) {
	if(argc == 0) {
		RETURNS_VOID shell_print_error("[INTERNAL] missing argv[0]\n");
		return ExitCodeImplementationError;
	}

	ShellSetupResult setup_res = shell_setup_repl(argv[0]);

	if(setup_res.type != ShellSetupResultTypeOk) {
		ASSERT(setup_res.type == ShellSetupResultTypeError);
		RETURNS_VOID shell_print_error("[INTERNAL]: " TSTR_FMT "\n",
		                               TSTR_FMT_ARGS(setup_res.data.error));

		tstr_free(&setup_res.data.error);

		return ExitCodeImplementationError;
	}

	ShellState state = setup_res.data.ok;

	bool ok = shell_setup_prompt(&state);
	if(!ok) {
		RETURNS_VOID shell_print_error("[INTERNAL]: prompt setup error\n");

		tstr_free(&setup_res.data.error);

		return ExitCodeImplementationError;
	}

	bool ok2 = shell_setup_completion(&state);
	if(!ok2) {
		RETURNS_VOID shell_print_error("[INTERNAL]: completion setup error\n");

		tstr_free(&setup_res.data.error);

		return ExitCodeImplementationError;
	}

	while(state.global.alive) {
		shell_repl_step(&state);
	}

	const ExitCode result = state.global.result;

	free_shell_state(&state);

	return result;
}

TMAP_HASH_FUNC_SIG(tstr, TStr) {
	return TMAP_HASH_BYTES(tstr_cstr(&key), tstr_len(&key));
}

TMAP_EQ_FUNC_SIG(tstr, TStr) {
	return tstr_eq(&key1, &key2);
}

TMAP_IMPLEMENT_MAP_TYPE(tstr, TStr, ShellCompletion, ShellCompletionHashMap)

TMAP_IMPLEMENT_MAP_TYPE(tstr, TStr, ShellVariable, ShellVariableHashMap)

TVEC_IMPLEMENT_VEC_TYPE(ShellJobState)
