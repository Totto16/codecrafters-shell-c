
#include "./input.h"

void free_shell_input_ok_result(ShellInputOkResult ok) {
  RETURNS_VOID free(ok.raw_data);
}

#define SHELL_INPUT_BUFFER_SIZE 1024

ShellInputResult shell_get_input(void) {

  char *data = NULL;
  bool end = false;

  data = malloc(SHELL_INPUT_BUFFER_SIZE + 1);

  if (data == NULL) {
    return (ShellInputResult){.type = ShellInputResultTypeError,
                              .data = {.err = ExitCodeOOM}};
  }

  data[0] = '\0';

  char *current_buffer_start = data;
  size_t current_buffer_size = SHELL_INPUT_BUFFER_SIZE + 1;

  while (true) {

    char *get_result =
        fgets(current_buffer_start, SHELL_INPUT_BUFFER_SIZE + 1, stdin);

    if (get_result == NULL) {
      if (feof(stdin) != 0) {
        return (ShellInputResult){.type = ShellInputResultTypeEOF, .data = {}};
      } else if (ferror(stdin) != 0) {
        RETURNS_VOID free(data);
        // TODO: print or return a more detailed error message
        return (ShellInputResult){.type = ShellInputResultTypeError,
                                  .data = {.err = ExitCodeGenericError}};
      } else {
        // TODO: print or return a more detailed error message
        return (ShellInputResult){.type = ShellInputResultTypeError,
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
        return (ShellInputResult){.type = ShellInputResultTypeError,
                                  .data = {.err = ExitCodeOOM}};
      }

      current_buffer_start = new_buffer + (current_buffer_start - data) +
                             (SHELL_INPUT_BUFFER_SIZE);
      data = new_buffer;

    } else {
      // if the last byte is NOT a \n, we have a valid input but EOF, so after
      // this command we are done
      const char last_char = current_buffer_start[read_bytes - 1];
      if (last_char != '\n') {
        end = true;
      }
      break;
    }
  }

  tstr_view view = tstr_view_from(data);

  if (tstr_view_ends_with(view, "\n")) {
    view = tstr_sub(view, 0, view.len - 1);
  }

  return (ShellInputResult){
      .type = ShellInputResultTypeOk,
      .data = {.ok = {.data = view, .raw_data = data, .end = end}}};
}
