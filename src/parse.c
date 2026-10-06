
#include "./parse.h"

typedef struct {
  ShellArgument program;
  ShellArguments args;
} ParsedProgram;

typedef struct {
  bool is_ok;
  union {
    ParsedProgram program;
    tstr error;
  } data;
} ParsedProgramResult;

static ParsedProgramResult shell_parse_input(tstr_view input) {

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
        return (ParsedProgramResult){
            .is_ok = false,
            .data = {.error = TSTR_LIT("Leading whitespace not supported")}};
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
            return (ParsedProgramResult){
                .is_ok = false,
                .data = {.error = TSTR_LIT("OOM error in internal parsing")}};
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
        return (ParsedProgramResult){
            .is_ok = false,
            .data = {.error = TSTR_LIT("OOM error in internal parsing")}};
      }
    }
  }

  return (ParsedProgramResult){.is_ok = true, .data = {.program = program}};
}

static void free_shell_argument(ShellArgument *arg) {
  tstr_free(&(arg->parsed));
}

static void free_parsed_program(ParsedProgram *program) {
  free_shell_arguments(&(program->args));
  free_shell_argument(&(program->program));
}

void free_shell_arguments(ShellArguments *args) {

  for (size_t i = 0; i < TVEC_LENGTH(ShellArgument, *args); ++i) {

    ShellArgument *arg = TVEC_GET_AT_MUT(ShellArgument, args, i);

    free_shell_argument(arg);
  }

  TVEC_FREE(ShellArgument, args);
}

void free_shell_command_not_found(ShellCommandNotFound *found) {
  tstr_free(&(found->name));
}

ShellParseDataResult shell_parse_data(tstr_view input) {

  ParsedProgramResult program_res = shell_parse_input(input);

  if (!program_res.is_ok) {
    // move program_res.data.error ownership to the ShellParseDataResult struct
    return (ShellParseDataResult){.type = ShellParseDataResultTypeError,
                                  .data = {.error = program_res.data.error}};
  }

  ParsedProgram program = program_res.data.program;

  if (tstr_len(&program.program.parsed) == 0) {
    return (ShellParseDataResult){.type = ShellParseDataResultEmpty,
                                  .data = {}};
  }

  const tstr *const prog_name = &program.program.parsed;
  const ShellArguments *const prog_args = &program.args;

  for (size_t i = 0; i < BUILTIN_FUNCTION_COUNT; ++i) {

    BuiltinCommand builtin_command = builtin_functions[i];

    if (tstr_eq_static_tstr(prog_name, builtin_command.name)) {

      ShellParseArgumentsResult args_res =
          builtin_command.validator.parse(prog_args);

      if (!args_res.is_ok) {
        free_parsed_program(&program);

        // move args_res.data.error ownership to the ShellParseDataResult struct
        return (ShellParseDataResult){.type = ShellParseDataResultTypeError,
                                      .data = {.error = args_res.data.error}};
      }

      if (args_res.data.args == NULL) {
        free_parsed_program(&program);

        return (ShellParseDataResult){
            .type = ShellParseDataResultTypeError,
            .data = {.error = TSTR_LIT("[INTERNAL] Parse error")}};
      }

      ShellCommandBuiltin builtin_args = {.command = builtin_command,
                                          .args = args_res.data.args};

      free_shell_argument(&program.program);

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
      .data = {.command_not_found = {.name = program.program.parsed}}};
}
