
#include "./parse.h"
#include "./command.h"

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

#define ADD_SHELL_ARGUMENT(argument)                                           \
  do {                                                                         \
    if (tstr_is_null(&program.program.parsed)) {                               \
      program.program = (argument);                                            \
    } else {                                                                   \
      TvecResult res = TVEC_PUSH(ShellArgument, &(program.args), (argument));  \
                                                                               \
      if (res !=                                                               \
          TvecResultOk) { /* NOLINT(readability-implicit-bool-conversion)*/    \
        return (ParsedProgramResult){                                          \
            .is_ok = false,                                                    \
            .data = {.error = TSTR_LIT("OOM error in internal parsing")}};     \
      }                                                                        \
    }                                                                          \
  } while (false)

#define ADD_TO_ACCUMULATOR(acc, argument)                                      \
  do {                                                                         \
    TStrResult res = tstr_cat_len(&(acc), (argument).data, (argument).len);    \
                                                                               \
    if (res != TStrResultOk) {                                                 \
      return (ParsedProgramResult){                                            \
          .is_ok = false,                                                      \
          .data = {.error = TSTR_LIT("OOM error in internal parsing")}};       \
    }                                                                          \
  } while (false)

typedef enum : uint8_t {
  ParseStateTypeNormal = 0,
  ParseStateTypeSingleQuote,
  ParseStateTypeDoubleQuote,
} ParseStateType;

typedef struct {
  tstr acc;
  size_t acc_start;
} ParseStateSingleQuote;

typedef struct {
  ParseStateType type;
  union {
    ParseStateSingleQuote single;
  } data;
} ParseState;

static ParsedProgramResult shell_parse_input(tstr_view input) {

  // TODO: proper parsing, escapeing, "   " , proper whitespace etc

  ParsedProgram program = {
      .program = {.parsed = tstr_null(), .original = TSTR_TSV("")},
      .args = TVEC_EMPTY(ShellArgument),
  };

  size_t start = 0;
  ParseState state = {.type = ParseStateTypeNormal, .data = {}};

  for (size_t i = 0; i < input.len; ++i) {

    const char character = input.data[i];

    switch (state.type) {
    case ParseStateTypeNormal: {
      if (isspace(character) != 0) {
        if (start == i) {
          // the next string starts with a whitespace, so skip this whitespace
          // as end delimiter
        } else {

          tstr_view orig =
              (tstr_view){.data = input.data + start, .len = i - start};
          ShellArgument argument = {.original = orig,
                                    .parsed = tstr_from_view(orig)};

          ADD_SHELL_ARGUMENT(argument);
        }

        start = i + 1;
      } else if (character == '\'') {
        state = (ParseState){
            .type = ParseStateTypeSingleQuote,
            .data = {.single = {.acc = tstr_from(""), .acc_start = start}}};

        if (start == i) {
          // no string before it, just advance start
        } else {
          // if we have some string before ' we need to set the accumulator to
          // it
          // the thing before this is a normal string without quotes, otherwise
          // it would already be consumed
          tstr_view orig =
              (tstr_view){.data = input.data + start, .len = i - start};
          ADD_TO_ACCUMULATOR(state.data.single.acc, orig);
        }
        start = i + 1;

      } else if (character == '"') {
        state = (ParseState){.type = ParseStateTypeDoubleQuote, .data = {}};

        // if we have some string before "" we need to set the accumulator to it
      }

      break;
    }
    case ParseStateTypeSingleQuote: {
      if (character == '\'') {

        tstr_view orig =
            (tstr_view){.data = input.data + start, .len = i - start};

        ADD_TO_ACCUMULATOR(state.data.single.acc, orig);

        tstr_view whole_orig =
            (tstr_view){.data = input.data + state.data.single.acc_start,
                        .len = i - state.data.single.acc_start + 1};

        ShellArgument argument = {.original = whole_orig,
                                  .parsed = state.data.single.acc};

        ADD_SHELL_ARGUMENT(argument);

        // we move out of the acc, so just setting it to null for safety
        state.data.single.acc = tstr_null();

        state = (ParseState){.type = ParseStateTypeNormal, .data = {}};

        start = i + 1;
      } else {
      }
      break;
    }
    case ParseStateTypeDoubleQuote: {
      return (ParsedProgramResult){
          .is_ok = false,
          .data = {.error = TSTR_LIT("[INTERNAL] TODO: Not Yet Implemented")}};
      break;
    }
    default: {
      return (ParsedProgramResult){
          .is_ok = false,
          .data = {.error = TSTR_LIT("[INTERNAL] wrong parse state")}};
    }
    }
  }

  if (state.type != ParseStateTypeNormal) {
    if (state.type == ParseStateTypeSingleQuote) {
      return (ParsedProgramResult){
          .is_ok = false,
          .data = {.error = TSTR_LIT("Unterminated single quote")}};
    } else if (state.type == ParseStateTypeDoubleQuote) {
      return (ParsedProgramResult){
          .is_ok = false,
          .data = {.error = TSTR_LIT("Unterminated double quote")}};
    } else {
      return (ParsedProgramResult){
          .is_ok = false,
          .data = {.error = TSTR_LIT("[INTERNAL] wrong parse state")}};
    }
  }

  if (start < input.len) {
    tstr_view orig =
        (tstr_view){.data = input.data + start, .len = input.len - start};
    ShellArgument argument = {.original = orig, .parsed = tstr_from_view(orig)};

    ADD_SHELL_ARGUMENT(argument);
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

  // TODO: hide behind some sort of set +d or similar debug option
  for (size_t i = 0; i < TVEC_LENGTH(ShellArgument, program.args); ++i) {

    const ShellArgument *const arg =
        TVEC_GET_AT(ShellArgument, &(program.args), i);

    int res =
        fprintf(stderr, "index: %zu\nvalue: " TSTR_FMT "\nraw: " TSTR_FMT "\n", i,
                TSTR_FMT_ARGS(arg->parsed), TSV_FMT_ARGS(arg->original));
    if (res < 0) {
      return (ShellParseDataResult){.type = ShellParseDataResultTypeError,
                                    .data = {.error = TSTR_LIT("IO Error")}};
    }
  }

  const tstr *const prog_name = &program.program.parsed;
  const ShellArguments *const prog_args = &program.args;

  CommandType command_type = determine_command_type(tstr_as_view(prog_name));

  switch (command_type.type) {
  case CommandTypeTypeBuiltin: {
    BuiltinCommand builtin_command = command_type.data.builtin;

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
  case CommandTypeTypeExecutable: {

    // we transfer ownership of command_type.data.executable.path to executable,
    // so no need to free that
    const ShellCommandExecutable executable = {
        .path = command_type.data.executable.path,
        .invoke_name = tstr_dup(&program.program.parsed),

    };

    free_shell_argument(&program.program);

    return (ShellParseDataResult){
        .type = ShellParseDataResultTypeCommand,
        .data = {.command = {.type = ShellCommandTypeExecutable,
                             .data = {.executable = executable},
                             .args = program.args}}};
  }
  default: {
    // TODO: report internal error
    break;
  }
  }

  free_shell_arguments(&program.args);

  return (ShellParseDataResult){
      .type = ShellParseDataResultTypeCommandNotFound,
      .data = {.command_not_found = {.name = program.program.parsed}}};
}
