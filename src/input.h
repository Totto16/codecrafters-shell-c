
#pragma once

#include <tstr/tstr.h>

#include "./shell.h"
#include "./utils.h"

#if !defined(USE_READLINE)
#error "USE_READLINE needs to be defined"
#elif USE_READLINE == 0
#include <histedit.h>
#elif USE_READLINE == 1
#include <readline/readline.h>
#else
#error "USE_READLINE has invalid value"
#endif

#if USE_READLINE == 0
typedef struct {
  tstr_view data;
} ShellInputOkResult;

#define INPUT_DATA_VIEW(input_data) ((input_data).data)

typedef struct {
  EditLine *el;
  tstr last_prompt_string;
} ShellInputState;
#else
typedef struct {
  char *data;
} ShellInputOkResult;

#define INPUT_DATA_VIEW(input_data) (tstr_view_from((input_data).data))

typedef struct {
  tstr prompt;
} ShellInputState;
#endif

void free_shell_input_ok_result(ShellInputOkResult ok);

typedef enum : uint8_t {
  ShellInputResultTypeOk = 0,
  ShellInputResultTypeError,
  ShellInputResultTypeEOF,
} ShellInputResultType;

typedef struct {
  ShellInputResultType type;
  union {
    ShellInputOkResult ok;
    tstr error;
  } data;
} ShellInputResult;

NODISCARD ShellInputResult shell_get_input(ShellInputState *state);
