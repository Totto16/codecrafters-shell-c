#include "./shell.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>

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

ExitCode shell_execute_repl(void) {

  ShellState state = {
      .alive = true,
      .result = ExitCodeSuccess,
  };

  while (state.alive) {
    shell_print_prompt(&state);
  }

  return ExitCodeSuccess;
}
