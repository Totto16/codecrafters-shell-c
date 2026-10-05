
#include "./parse.h"

typedef struct {
  ShellArgument program;
  ShellArguments args;
} ParsedProgram;

// TODO: return eventual errors
static ParsedProgram shell_parse_input(tstr_view input) {

  // TODO: proper parsing, escapeing, "   " , proper whitespace etc

  ParsedProgram program = {
      .program = {.parsed = TSTR_LIT(""), .original = TSTR_TSV("")},
      .args = TVEC_EMPTY(ShellArgument),
  };

  bool first = true;
  size_t start = 0;

  for (size_t i = 0; i < input.len; ++i) {

    const char character = input.data[i];

    if (isspace(character) != 0) {
      if (i == 0) {
        // TODO: should we support leading whitespace
        exit(121);
      }

      if (start != i) {

        tstr_view orig =
            (tstr_view){.data = input.data + start, .len = i - start};
        ShellArgument argument = {.original = orig,
                                  .parsed = tstr_from_view(orig)};

        if (first) {
          program.program = argument;
          first = false;
        } else {
          TvecResult res = TVEC_PUSH(ShellArgument, &(program.args), argument);

          if (res !=
              TvecResultOk) { // NOLINT(readability-implicit-bool-conversion)
            // TODO
            exit(16);
          }
        }
      }

      start = i + 1;
    }
  }

  if (start < input.len) {
    tstr_view orig =
        (tstr_view){.data = input.data + start, .len = input.len - start};
    ShellArgument argument = {.original = orig, .parsed = tstr_from_view(orig)};

    if (first) {
      program.program = argument;
      first = false;
    } else {
      TvecResult res = TVEC_PUSH(ShellArgument, &(program.args), argument);

      if (res != TvecResultOk) { // NOLINT(readability-implicit-bool-conversion)
        // TODO
        exit(16);
      }
    }
  }

  return program;
}

static void free_shell_argument(ShellArgument *arg) {
  tstr_free(&(arg->parsed));
}

static void free_shell_arguments(ShellArguments *args) {

  for (size_t i = 0; i < TVEC_LENGTH(ShellArgument, *args); ++i) {

    ShellArgument *arg = TVEC_GET_AT_MUT(ShellArgument, args, i);

    free_shell_argument(arg);
  }

  TVEC_FREE(ShellArgument, args);
}

ShellParseDataResult shell_parse_data(tstr_view input) {

  ParsedProgram program = shell_parse_input(input);

  if (tstr_len(&program.program.parsed) == 0) {

    return (ShellParseDataResult){.type = ShellParseDataResultEmpty,
                                  .data = {}};
  }

  tstr prog_name = program.program.parsed;

  for (size_t i = 0; i < BUILTIN_FUNCTION_COUNT; ++i) {

    BuiltinCommand command = builtin_functions[i];

    if (tstr_eq_static_tstr(&prog_name, command.name)) {

      ShellParseArgumentsResult args_res =
          command.validator.parse(&program.args);

      if (!args_res.is_ok) {
        return (ShellParseDataResult){.type = ShellParseDataResultTypeError,
                                      .data = {.error = args_res.data.error}};
      }

      if (args_res.data.args == NULL) {
        return (ShellParseDataResult){
            .type = ShellParseDataResultTypeError,
            .data = {.error = TSTR_LIT("[INTERNAL] Parse error")}};
      }

      ShellCommandBuiltin builtin_args = {.command = command,
                                          .args = args_res.data.args};

      return (ShellParseDataResult){
          .type = ShellParseDataResultTypeCommand,
          .data = {.command = {.type = ShellCommandTypeBuiltin,
                               .data = {.builtin = builtin_args},
                               .args = program.args}}};
    }
  }

  free_shell_arguments(&program.args);

  return (ShellParseDataResult){
      .type = ShellParseDataResultTypeCommandNotFound,
      .data = {.command_not_found = {.name = prog_name}}};
}
