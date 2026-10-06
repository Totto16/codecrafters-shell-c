#include "./shell.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <tstr/tstr.h>

#include "./builtins.h"
#include "./input.h"
#include "./parse.h"

static void shell_setup_repl(void) {
  // Flush after every printf
  RETURNS_VOID setbuf(stdout, NULL);
}

typedef struct {
  ExitCode exit_code;
} CommandState;

typedef struct {
  bool alive;
  ExitCode result;
  CommandState command_state;
} ShellState;

static tstr shell_get_prompt(void) {

  // TODO. support setting the prompt dynamically

  return TSTR_LIT("$");
}

static bool shell_print_prompt(const ShellState *const state) {
  tstr prompt = shell_get_prompt();

  int res = fprintf(stdout, TSTR_FMT " ", TSTR_FMT_ARGS(prompt));

  tstr_free(&prompt);

  if (res < 0) {
    return false;
  }

  return true;
}

static void shell_print_error(const char *error) {
  int res = fprintf(stderr, "%s\n", error);
  if (res < 0) {
    // Note: we already have an error, so no reporting possible here
  }
}

static void shell_exit(ShellState *state, ExitCode code) {
  state->alive = false;
  state->result = code;
}

static void set_command_exit_code(ShellState *state, ExitCode code) {
  state->command_state.exit_code = code;
}

void shell_repl_step(ShellState *state) {
  bool ok = shell_print_prompt(state);
  if (!ok) {
    RETURNS_VOID shell_exit(state, ExitCodeGenericError);

    RETURNS_VOID shell_print_error("[INTERNAL] prompt print error");
    return;
  }
  ShellInputResult input = shell_get_input();

  if (!input.is_ok) {
    RETURNS_VOID shell_exit(state, input.data.err);

    RETURNS_VOID shell_print_error("Can't read input");
    return;
  }

  ShellInputOkResult data = input.data.ok;
  ShellParseDataResult parse = shell_parse_data(data.data);

  switch (parse.type) {
  case ShellParseDataResultTypeCommand: {
    const ShellCommand command = parse.data.command;
    switch (command.type) {
    case ShellCommandTypeBuiltin: {
      const ShellCommandBuiltin builtin = command.data.builtin;

      // special cases handle
      if (builtin.command.invoke == exit_invoke) {

        // TODO: Use trtti
        const ExitData *const exit_data = builtin.args;

        if (!exit_data->has_code) {

          RETURNS_VOID shell_exit(state, state->command_state.exit_code);
        } else {
          RETURNS_VOID shell_exit(state, exit_data->code);
        }

      } else {

        ExitCode command_exit_code = builtin.command.invoke(builtin.args);
        set_command_exit_code(state, command_exit_code);
      }

      builtin.command.validator.free(builtin.args);

      break;
    }

    default: {
      RETURNS_VOID shell_exit(state, ExitCodeGenericError);

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
      RETURNS_VOID shell_exit(state, ExitCodeGenericError);

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
      RETURNS_VOID shell_exit(state, ExitCodeGenericError);

      RETURNS_VOID shell_print_error("[INTERNAL] output error");
      break;
    }
    set_command_exit_code(state, ExitCodeGenericError);
    break;
  }
  default: {
    RETURNS_VOID shell_exit(state, ExitCodeGenericError);

    RETURNS_VOID shell_print_error("[INTERNAL] Invalid parse result");
    break;
  }
  }

  RETURNS_VOID free_shell_input_ok_result(data);
}

ExitCode shell_execute_repl(void) {

  ShellState state = {
      .alive = true,
      .result = ExitCodeSuccess,
      .command_state = {.exit_code = ExitCodeSuccess},

  };

  while (state.alive) {
    shell_repl_step(&state);
  }

  return state.result;
}
