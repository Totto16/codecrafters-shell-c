
#include "./input.h"
#include "./utils.h"

#include <readline/readline.h>

void free_shell_input_ok_result(ShellInputOkResult ok) {
  RETURNS_VOID rl_free(ok.data);
}

#define SHELL_INPUT_BUFFER_SIZE 1024

ShellInputResult shell_get_input(const char *const prompt) {
  char *line = readline(prompt);

  if (line == NULL) {
    return (ShellInputResult){.type = ShellInputResultTypeEOF, .data = {}};
  }

  return (ShellInputResult){.type = ShellInputResultTypeOk,
                            .data = {.ok = {.data = line}}};
}
