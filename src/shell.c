#include "./shell.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void shell_setup_repl() {
  // Flush after every printf
  setbuf(stdout, NULL);
}

typedef struct {
  bool alive;
  ExitCode result;
} ShellState;

static void shell_print_prompt(const ShellState *const state) {
  fprintf(stdout, "$ ");
}

static void shell_print_error(const char *error) {
  fprintf(stderr, "%s\n", error);
}

typedef struct {
  char *data; // TODO: use tstr which has length instead of needing strlen()
              // everytime we parse
} ShellInputOkResult;

typedef struct {
  bool is_ok;
  union {
    ExitCode err;
    ShellInputOkResult ok;
  } data;
} ShellInputResult;

#define SHELL_INPUT_BUFFER_SIZE 1024

static ShellInputResult shell_get_input(void) {

  ShellInputOkResult result = {.data = NULL};

  result.data = malloc(SHELL_INPUT_BUFFER_SIZE + 1);

  if (result.data == NULL) {
    return (ShellInputResult){.is_ok = false, .data = {.err = ExitCodeOOM}};
  }

  result.data[0] = '\0';

  char *current_buffer_start = result.data;
  size_t current_buffer_size = SHELL_INPUT_BUFFER_SIZE + 1;

  while (true) {

    char *get_result =
        fgets(current_buffer_start, SHELL_INPUT_BUFFER_SIZE + 1, stdin);

    if (get_result == NULL) {
      if (feof(stdin) != 0) {
        // return the data up until now
        break;
      } else if (ferror(stdin) != 0) {
        free(result.data);
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
        // TODO: use tstr view and just chop the last byte off;
        current_buffer_start[SHELL_INPUT_BUFFER_SIZE] = '\0';
        break;
      }

      // we have more data
      current_buffer_size += SHELL_INPUT_BUFFER_SIZE;
      char *new_buffer = realloc(result.data, current_buffer_size);

      if (new_buffer == NULL) {
        free(result.data);
        return (ShellInputResult){.is_ok = false, .data = {.err = ExitCodeOOM}};
      }

      current_buffer_start = new_buffer + (current_buffer_start - result.data) +
                             (SHELL_INPUT_BUFFER_SIZE);
      result.data = new_buffer;

    } else {
      // we have encountered a \n
      break;
    }
  }

  size_t result_len = strlen(result.data);
  const char last_char = result.data[result_len - 1];
  if (last_char == '\n') {
    // TODO: use tstr view and just chop the last byte off;
    result.data[result_len - 1] = '\0';
  }

  return (ShellInputResult){.is_ok = true, .data = {.ok = result}};
}

typedef enum : uint8_t {
  ShellParseDataResultTypeCommand = 0,
  ShellParseDataResultTypeCommandNotFound,
} ShellParseDataResultType;

typedef struct {
  // TODO: use tstr
  char *name;
} ShellCommandNotFound;

typedef struct {
  // TODO
  int todo;
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

  return (ShellParseDataResult){
      .type = ShellParseDataResultTypeCommandNotFound,
      .data = {.command_not_found = {.name = input.data}}};
}

ExitCode shell_execute_repl(void) {

  ShellState state = {
      .alive = true,
      .result = ExitCodeSuccess,
  };

  while (state.alive) {
    shell_print_prompt(&state);
    ShellInputResult input = shell_get_input();

    if (!input.is_ok) {
      state.alive = false;
      state.result = input.data.err;

      shell_print_error("Can't read input");
      break;
    }

    ShellInputOkResult data = input.data.ok;
    ShellParseDataResult parse = shell_parse_data(data);

    switch (parse.type) {
    case ShellParseDataResultTypeCommand: {
      state.alive = false;
      state.result = ExitCodeGenericError;

      shell_print_error("[INTERNAL] Not Implemented yet");
      break;
    }
    case ShellParseDataResultTypeCommandNotFound: {
      fprintf(stderr, "%s: command not found\n",
              parse.data.command_not_found.name);
      break;
    }
    default: {
      state.alive = false;
      state.result = ExitCodeGenericError;

      shell_print_error("[INTERNAL] Invalid parse result");
      break;
    }
    }
  }

  return state.result;
}
