#include "./shell.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

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
  CommandState command_state;
  const bool interactive;
  tstr cwd;
  ShellInputState input_state;
} ShellState;

static tstr shell_get_prompt(const ShellState *const state) {

  // TODO. support setting the prompt dynamically
  UNUSED(state);

  return TSTR_LIT("$ ");
}

static void shell_print_errors(const char **errors, size_t count) {
  for (size_t i = 0; i < count; ++i) {

    int res = fprintf(stderr, "%s", errors[i]);
    if (res < 0) {
      fflush(stderr);
      // Note: we already have an error, so no reporting possible here
      return;
    }
  }

  int res = fprintf(stderr, "\n");
  if (res < 0) {
    // Note: we already have an error, so no reporting possible here
  }

  fflush(stderr);
}

static void shell_print_error(const char *error) {
  const char *errors[] = {error};
  shell_print_errors(errors, ARRAY_SIZE(errors));
}

static void shell_exit(ShellState *state, ExitCode code) {
  if (state->alive) {
    // ignore duplicate exits, the first one sets the code, the second one is
    // ignore (eg if we call "exit 22" and get EOF after that, it should result
    // in 22)
    state->alive = false;
    state->result = code;
  }
}

static void set_command_exit_code(ShellState *state, ExitCode code) {
  state->command_state.exit_code = code;
}

static void free_shell_command_executable(ShellCommandExecutable *executable) {
  tstr_free(&(executable->invoke_name));
  tstr_free(&(executable->path));
}

static void shell_virtual_fn_exit(shell_context ctx, const ExitData *data) {

  ShellState *state = ctx;

  if (!data->has_code) {

    RETURNS_VOID shell_exit(state, state->command_state.exit_code);
  } else {
    RETURNS_VOID shell_exit(state, data->code);
  }
}

static tstr_view
shell_virtual_fn_get_current_working_directory(shell_context ctx) {

  ShellState *state = ctx;

  return tstr_as_view(&(state->cwd));
}

static bool
shell_virtual_fn_set_current_working_directory(shell_context ctx,
                                               const tstr *normalized_cwd) {

  ShellState *state = ctx;

  if (!path_is_absolute(tstr_as_view(normalized_cwd))) {
    return false;
  }

  if (!path_is_normalized(tstr_as_view(normalized_cwd))) {
    return false;
  }

  if (!path_exists_and_is_dir(tstr_as_view(normalized_cwd))) {
    return false;
  }

  int res = chdir(tstr_cstr(normalized_cwd));
  if (res < 0) {
    return false;
  }

  tstr_free(&(state->cwd));
  state->cwd = tstr_dup(normalized_cwd);

  return true;
}

static ShellVirtualFunctions g_shell_functions = {
    .exit = shell_virtual_fn_exit,
    .get_current_working_directory =
        shell_virtual_fn_get_current_working_directory,
    .set_current_working_directory =
        shell_virtual_fn_set_current_working_directory,
};
#if SHELL_USE_EDIT_LIBRARY == 0
static void shell_cleanup_old_prompt(ShellState *state) {
  tstr_free(&state->input_state.last_prompt_string);
}
static void shell_set_new_prompt(ShellState *state, tstr prompt) {
  shell_cleanup_old_prompt(state);
  state->input_state.last_prompt_string = prompt;
}

static bool shell_process_input_state(ShellState *state) {
  shell_cleanup_old_prompt(state);
  return true;
}
#else
static bool shell_set_prompt_to_input_state(ShellState *state) {
  tstr prompt;
  if (state->interactive) {
    prompt = shell_get_prompt(state);

    if (tstr_is_null(&prompt)) {
      RETURNS_VOID shell_exit(state, ExitCodeGenericError);

      RETURNS_VOID shell_print_error("[INTERNAL] prompt print error");
      return false;
    }
  } else {
    prompt = tstr_null();
  }

  state->input_state.prompt = prompt;

  return true;
}

static bool shell_process_input_state(ShellState *state) {

  return shell_set_prompt_to_input_state(state);
}
#endif

void shell_repl_step(ShellState *state) {

  if (!shell_process_input_state(state)) {

    RETURNS_VOID shell_exit(state, ExitCodeGenericError);

    RETURNS_VOID shell_print_error("[INTERNAL] prompt print error");
    return;
  }

  ShellInputResult input = shell_get_input(&(state->input_state));

  if (input.type == ShellInputResultTypeEOF) {
    RETURNS_VOID shell_exit(state, state->command_state.exit_code);
    return;
  }

  if (input.type != ShellInputResultTypeOk) {
    tstr error = TSTR_LIT("Can't read input");
    if (input.type == ShellInputResultTypeError) {
      tstr_free(&error);
      error = input.data.error;
    }

    RETURNS_VOID shell_exit(state, ExitCodeImplementationError);

    RETURNS_VOID shell_print_error(tstr_cstr(&error));
    tstr_free(&error);
    return;
  }

  ShellHandle shell_handle = {.context = state, .function = g_shell_functions};

  ShellInputOkResult data = input.data.ok;
  ShellParseDataResult parse =
      shell_parse_data(INPUT_DATA_VIEW(data), shell_handle);

  switch (parse.type) {
  case ShellParseDataResultTypeCommand: {
    const ShellCommand command = parse.data.command;
    switch (command.type) {
    case ShellCommandTypeBuiltin: {
      const ShellCommandBuiltin builtin = command.data.builtin;

      ExitCode command_exit_code =
          builtin.command.invoke(builtin.args, shell_handle);
      set_command_exit_code(state, command_exit_code);

      builtin.command.validator.free(builtin.args);

      break;
    }
    case ShellCommandTypeExecutable: {
      ShellCommandExecutable executable = command.data.executable;

      ExitCode command_exit_code = invoke_executable(
          &executable.path, &executable.invoke_name, &command.args);
      set_command_exit_code(state, command_exit_code);

      free_shell_command_executable(&executable);

      break;
    }
    default: {
      RETURNS_VOID shell_exit(state, ExitCodeImplementationError);

      RETURNS_VOID shell_print_error("[INTERNAL] Invalid command type");
      break;
    }
    }

    free_shell_arguments(&parse.data.command.args);

    break;
  }
  case ShellParseDataResultTypeCommandNotFound: {
    int res = fprintf(stderr, TSTR_FMT ": command not found\n",
                      TSTR_FMT_ARGS(parse.data.command_not_found.name));

    free_shell_command_not_found(&parse.data.command_not_found);

    if (res < 0) {
      RETURNS_VOID shell_exit(state, ExitCodeIOError);

      RETURNS_VOID shell_print_error("[INTERNAL] output error");
      break;
    }
    set_command_exit_code(state, ExitCodeCommandNotFound);
    break;
  }
  case ShellParseDataResultEmpty: {
    break;
  }
  case ShellParseDataResultTypeError: {
    int res = fprintf(stderr, TSTR_FMT "\n", TSTR_FMT_ARGS(parse.data.error));
    tstr_free(&parse.data.error);

    if (res < 0) {
      RETURNS_VOID shell_exit(state, ExitCodeIOError);

      RETURNS_VOID shell_print_error("[INTERNAL] output error");
      break;
    }
    set_command_exit_code(state, ExitCodeGenericError);
    break;
  }
  default: {
    RETURNS_VOID shell_exit(state, ExitCodeImplementationError);

    RETURNS_VOID shell_print_error("[INTERNAL] Invalid parse result");
    break;
  }
  }

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

static ShellSetupResult shell_setup_repl(const char *const program_name) {
  // Flush after every printf
  RETURNS_VOID setbuf(stdout, NULL);

#if SHELL_USE_EDIT_LIBRARY == 0
  EditLine *el = el_init(program_name, stdin, stdout, stderr);
  if (el == NULL) {
    return (ShellSetupResult){
        .type = ShellSetupResultTypeError,
        .data = {.error = TSTR_LIT("libedit init error")}};
  }

  // TODO: setup custom libedit behaviour
  //  this includes: signal handling, tab completion, history

  ShellInputState input_state = {.el = el};
#else
  UNUSED(program_name);
  int res = rl_initialize();
  if (res != 0) {
    return (ShellSetupResult){
        .type = ShellSetupResultTypeError,
        .data = {.error = TSTR_LIT("readline init error")}};
  }

  // TODO: setup custom readline behaviour
  //  this includes: signal handling, tab completion, history

  ShellInputState input_state = {.prompt = tstr_null()};
#endif

  bool interactive = isatty(STDIN_FILENO) == 1;

  tstr cwd = path_get_cwd();

  if (tstr_is_null(&cwd)) {
    return (ShellSetupResult){.type = ShellSetupResultTypeError,
                              .data = {.error = TSTR_LIT("ced get error")}};
  }

  ShellState state = {
      .alive = true,
      .result = ExitCodeSuccess,
      .command_state = {.exit_code = ExitCodeSuccess},
      .interactive = interactive,
      .cwd = cwd,
      .input_state = input_state,
  };

  return (ShellSetupResult){.type = ShellSetupResultTypeOk,
                            .data = {.ok = state}};
}

static void shell_destroy_state(ShellState *state) {

  tstr_free(&(state->cwd));
#if SHELL_USE_EDIT_LIBRARY == 0
  el_end(state->input_state.el);
  shell_cleanup_old_prompt(state);
#endif
}

#if SHELL_USE_EDIT_LIBRARY == 0
typedef const char *(*el_pfunct_t)(EditLine *);

NODISCARD static bool el_set_prompt(EditLine *e, el_pfunct_t fn) {
  return el_set(e, EL_PROMPT, fn) == 0;
}

NODISCARD static bool el_set_clientdata(EditLine *e, void *data) {
  return el_set(e, EL_CLIENTDATA, data) == 0;
}

NODISCARD static void *el_get_clientdata(EditLine *e) {
  void *data = NULL;
  int res = el_get(e, EL_CLIENTDATA, &data);
  if (res != 0) {
    return NULL;
  }

  return data;
}

NODISCARD static const char *shell_get_prompt_el_cb(EditLine *el) {

  ShellState *state = el_get_clientdata(el);
  if (state == NULL) {
    goto err;
  }

  tstr prompt;
  if (state->interactive) {
    prompt = shell_get_prompt(state);

    if (tstr_is_null(&prompt)) {
      goto err;
    }
  } else {
    prompt = TSTR_LIT("");
  }

  const char *prompt_str = tstr_cstr(&prompt);

  if (prompt_str == NULL) {
    goto err;
  }

  shell_set_new_prompt(state, prompt);

  return prompt_str;

err:
  RETURNS_VOID shell_exit(state, ExitCodeImplementationError);

  RETURNS_VOID shell_print_error("[INTERNAL] prompt print error");
  return "$ ";
}
#endif

NODISCARD static bool shell_setup_prompt(ShellState *state) {

#if SHELL_USE_EDIT_LIBRARY == 0
  bool ok = el_set_clientdata(state->input_state.el, state);
  if (!ok) {
    return false;
  }
  ok = el_set_prompt(state->input_state.el, shell_get_prompt_el_cb);
  if (!ok) {
    return false;
  }
  // TODO
  // el_set(EL_EDITOR, "emacs" "vi")
  return true;
#else

  return shell_set_prompt_to_input_state(state);
#endif
}

ExitCode shell_execute_repl(const size_t argc, char *const *const argv) {
  if (argc == 0) {
    RETURNS_VOID shell_print_error("[INTERNAL] missing argv[0]");
    return ExitCodeImplementationError;
  }

  ShellSetupResult setup_res = shell_setup_repl(argv[0]);

  if (setup_res.type != ShellSetupResultTypeOk) {
    const char *errors[] = {"[INTERNAL]: ", tstr_cstr(&setup_res.data.error)};
    RETURNS_VOID shell_print_errors(errors, ARRAY_SIZE(errors));

    tstr_free(&setup_res.data.error);

    return ExitCodeImplementationError;
  }

  ShellState state = setup_res.data.ok;

  bool ok = shell_setup_prompt(&state);
  if (!ok) {
    RETURNS_VOID shell_print_error("[INTERNAL]: el prompt set error");

    tstr_free(&setup_res.data.error);

    return ExitCodeImplementationError;
  }

  while (state.alive) {
    shell_repl_step(&state);
  }

  shell_destroy_state(&state);

  return state.result;
}
