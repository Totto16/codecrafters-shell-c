#include "./shell.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <tstr/tstr.h>

#define RETURNS_VOID (void)

static void shell_setup_repl() {
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

static void shell_print_prompt(const ShellState *const state) {
  int res = fprintf(stdout, "$ ");
  if (res < 0) {
    // TODO report internal failure
  }
}

static void shell_print_error(const char *error) {
  int res = fprintf(stderr, "%s\n", error);
  if (res < 0) {
    // TODO report internal failure
  }
}

typedef struct {
  char *raw_data;
  tstr_view data;
} ShellInputOkResult;

static void free_shell_input_ok_result(ShellInputOkResult ok) {
  RETURNS_VOID free(ok.raw_data);
}

typedef struct {
  bool is_ok;
  union {
    ExitCode err;
    ShellInputOkResult ok;
  } data;
} ShellInputResult;

#define SHELL_INPUT_BUFFER_SIZE 1024

static ShellInputResult shell_get_input(void) {

  char *data = NULL;

  data = malloc(SHELL_INPUT_BUFFER_SIZE + 1);

  if (data == NULL) {
    return (ShellInputResult){.is_ok = false, .data = {.err = ExitCodeOOM}};
  }

  data[0] = '\0';

  char *current_buffer_start = data;
  size_t current_buffer_size = SHELL_INPUT_BUFFER_SIZE + 1;

  while (true) {

    char *get_result =
        fgets(current_buffer_start, SHELL_INPUT_BUFFER_SIZE + 1, stdin);

    if (get_result == NULL) {
      if (feof(stdin) != 0) {
        // return the data up until now
        break;
      } else if (ferror(stdin) != 0) {
        RETURNS_VOID free(data);
        // TODO: print or return a more detailed error message
        return (ShellInputResult){.is_ok = false,
                                  .data = {.err = ExitCodeGenericError}};
      } else {
        // TODO: print or return a more detailed error message
        return (ShellInputResult){.is_ok = false,
                                  .data = {.err = ExitCodeGenericError}};
      }
    }

    size_t read_bytes = strlen(current_buffer_start);

    if (read_bytes >= SHELL_INPUT_BUFFER_SIZE) {
      // if the last byte is a \n, we have exactly that amount of data,
      // otherwise we have more data
      const char last_char = current_buffer_start[SHELL_INPUT_BUFFER_SIZE];
      if (last_char == '\n') {
        break;
      }

      // we have more data
      current_buffer_size += SHELL_INPUT_BUFFER_SIZE;
      char *new_buffer = realloc(data, current_buffer_size);

      if (new_buffer == NULL) {
        free(data);
        return (ShellInputResult){.is_ok = false, .data = {.err = ExitCodeOOM}};
      }

      current_buffer_start = new_buffer + (current_buffer_start - data) +
                             (SHELL_INPUT_BUFFER_SIZE);
      data = new_buffer;

    } else {
      // we have encountered a \n
      break;
    }
  }

  tstr_view view = tstr_view_from(data);

  if (tstr_view_ends_with(view, "\n")) {
    view = tstr_sub(view, 0, view.len - 1);
  }

  return (ShellInputResult){.is_ok = true,
                            .data = {.ok = {.data = view, .raw_data = data}}};
}

typedef enum : uint8_t {
  ShellParseDataResultTypeCommand = 0,
  ShellParseDataResultTypeCommandNotFound,
} ShellParseDataResultType;

typedef struct {
  tstr_view name;
} ShellCommandNotFound;

typedef enum : uint8_t {
  ShellCommandTypeBuiltin = 0,
} ShellCommandType;

typedef enum : uint8_t {
  ShellBuiltinCommandExit = 0,
  ShellBuiltinCommandEcho,
} ShellBuiltinCommand;

// TODO: support multiple
typedef struct {
  tstr_view all;
} ShellArguments;

typedef struct {
  ShellCommandType type;
  union {
    ShellBuiltinCommand builtin;
  } data;
  ShellArguments args;
} ShellCommand;

typedef struct {
  ShellParseDataResultType type;
  union {
    ShellCommandNotFound command_not_found;
    ShellCommand command;
  } data;
} ShellParseDataResult;

static ShellParseDataResult shell_parse_data(ShellInputOkResult input) {

  // TODO: parse the data for real

  // TODO: proper parsing
  // TODO: '"exit"' should also resolve to this and 'exit 12' also, but with
  // arguments
  if (tstr_view_eq(input.data, "exit")) {
    return (ShellParseDataResult){
        .type = ShellParseDataResultTypeCommand,
        .data = {.command = {.type = ShellCommandTypeBuiltin,
                             .data = {.builtin = ShellBuiltinCommandExit},
                             .args = {.all = TSTR_TSV("")}}}};
  }

  // TODO: better detection of echo and it's arguments
  if (tstr_view_starts_with(input.data, "echo")) {
    ShellArguments args = {.all = TSTR_TSV("")};
    if (tstr_view_starts_with(input.data, "echo ")) {
      args = (ShellArguments){.all = tstr_sub_until_end(input.data, 5)};
    }

    return (ShellParseDataResult){
        .type = ShellParseDataResultTypeCommand,
        .data = {.command = {.type = ShellCommandTypeBuiltin,
                             .data = {.builtin = ShellBuiltinCommandEcho},
                             .args = args}}};
  }

  return (ShellParseDataResult){
      .type = ShellParseDataResultTypeCommandNotFound,
      .data = {.command_not_found = {.name = input.data}}};
}

static void shell_exit(ShellState *state, ExitCode code) {
  state->alive = false;
  state->result = code;
}

static void set_command_exit_code(ShellState *state, ExitCode code) {
  state->command_state.exit_code = code;
}

ExitCode shell_execute_repl(void) {

  ShellState state = {
      .alive = true,
      .result = ExitCodeSuccess,
      .command_state = {.exit_code = ExitCodeSuccess},

  };

  while (state.alive) {
    RETURNS_VOID shell_print_prompt(&state);
    ShellInputResult input = shell_get_input();

    if (!input.is_ok) {
      RETURNS_VOID shell_exit(&state, input.data.err);

      RETURNS_VOID shell_print_error("Can't read input");
      break;
    }

    ShellInputOkResult data = input.data.ok;
    ShellParseDataResult parse = shell_parse_data(data);

    switch (parse.type) {
    case ShellParseDataResultTypeCommand: {
      const ShellCommand command = parse.data.command;
      switch (command.type) {
      case ShellCommandTypeBuiltin: {
        const ShellBuiltinCommand builtin = command.data.builtin;
        switch (builtin) {
        case ShellBuiltinCommandExit: {
          // TODO: get and validate arguments
          RETURNS_VOID shell_exit(&state, ExitCodeSuccess);
          break;
        }
        case ShellBuiltinCommandEcho: {
          int res =
              fprintf(stdout, TSTR_FMT "\n", TSV_FMT_ARGS(command.args.all));
          set_command_exit_code(&state, res < 0 ? ExitCodeGenericError
                                                : ExitCodeSuccess);
          break;
        }
        default: {
          RETURNS_VOID shell_exit(&state, ExitCodeGenericError);

          RETURNS_VOID shell_print_error(
              "[INTERNAL] Invalid builtin command type");
          break;
        }
        }

        break;
      }
      default: {
        RETURNS_VOID shell_exit(&state, ExitCodeGenericError);

        RETURNS_VOID shell_print_error("[INTERNAL] Invalid command type");
        break;
      }
      }
      break;
    }
    case ShellParseDataResultTypeCommandNotFound: {
      int res = fprintf(stderr, TSTR_FMT ": command not found\n",
                        TSV_FMT_ARGS(parse.data.command_not_found.name));
      if (res < 0) {
        // TODO: handle res
      }
      set_command_exit_code(&state, ExitCodeCommandNotFound);
      break;
    }
    default: {
      RETURNS_VOID shell_exit(&state, ExitCodeGenericError);

      RETURNS_VOID shell_print_error("[INTERNAL] Invalid parse result");
      break;
    }
    }

    RETURNS_VOID free_shell_input_ok_result(data);
  }

  return state.result;
}
