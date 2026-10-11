
#pragma once

#include <tstr/tstr.h>

#include "./shell.h"
#include "./utils.h"

#if !defined(SHELL_USE_EDIT_LIBRARY)
	#error "SHELL_USE_EDIT_LIBRARY needs to be defined"
#elif SHELL_USE_EDIT_LIBRARY == 0
	#include <histedit.h>
#elif SHELL_USE_EDIT_LIBRARY == 1
	#include <readline/readline.h>
	#include <readline/history.h>
#elif SHELL_USE_EDIT_LIBRARY == 2
// NOthing to include
#else
	#error "SHELL_USE_EDIT_LIBRARY has invalid value"
#endif

#if SHELL_USE_EDIT_LIBRARY == 0
typedef struct {
	tstr_view data;
} ShellInputOkResult;

	#define INPUT_DATA_VIEW(input_data) ((input_data).data)

typedef struct {
	EditLine* el;
	tstr last_prompt_string;
} ShellInputState;
#elif SHELL_USE_EDIT_LIBRARY == 1
typedef struct {
	char* data;
} ShellInputOkResult;

	#define INPUT_DATA_VIEW(input_data) (tstr_view_from((input_data).data))

typedef struct {
	tstr prompt;
} ShellInputState;
#else
typedef struct {
	char* raw_data;
	tstr_view data;
	bool end;
} ShellInputOkResult;

	#define INPUT_DATA_VIEW(input_data) ((input_data).data)

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

NODISCARD ShellInputResult shell_get_input(ShellInputState* state);
