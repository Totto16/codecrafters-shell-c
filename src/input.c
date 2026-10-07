
#include "./input.h"
#include "./utils.h"

void free_shell_input_ok_result(ShellInputOkResult ok) {
  UNUSED(ok);
  // NOte. no need to free anything, the line buffer is managed by the global
  // instance of  EditLine *el
}
#if USE_READLINE == 0
ShellInputResult shell_get_input(ShellInputState *const state) {
  int count = 0;
  const char *line = el_gets(state->el, &count);

  if (line == NULL) {
    return (ShellInputResult){.type = ShellInputResultTypeEOF, .data = {}};
  }

  if (count < 0) {
    return (ShellInputResult){.type = ShellInputResultTypeError,
                              .data = {.error = TSTR_LIT("count is negative")}};
  }

  return (ShellInputResult){
      .type = ShellInputResultTypeOk,
      .data = {.ok = {.data = (tstr_view){.data = line, .len = count}}}};
}

#else
ShellInputResult shell_get_input(ShellInputState *const state) {
  // NOTE: prompt can be NULL here
  char *line = readline(tstr_cstr(&state->prompt));

  if (line == NULL) {
    return (ShellInputResult){.type = ShellInputResultTypeEOF, .data = {}};
  }

  return (ShellInputResult){.type = ShellInputResultTypeOk,
                            .data = {.ok = {.data = line}}};
}
#endif
