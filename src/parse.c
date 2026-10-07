
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

#define ADD_TO_ACCUMULATOR(argument, add_raw_span)                             \
  do {                                                                         \
    TStrResult res =                                                           \
        tstr_cat_len(&(acc.str), (argument).data, (argument).len);             \
                                                                               \
    if (res != TStrResultOk) {                                                 \
      return (ParsedProgramResult){                                            \
          .is_ok = false,                                                      \
          .data = {.error = TSTR_LIT("OOM error in internal parsing")}};       \
    }                                                                          \
    acc.raw.len += add_raw_span;                                               \
  } while (false)

typedef enum : uint8_t {
  ParseStateTypeNormal = 0,
  ParseStateTypeSingleQuote,
  ParseStateTypeDoubleQuote,
} ParseStateType;

typedef struct {
  tstr str;
  tstr_view raw;
} ParseStateAccumulator;

typedef struct {
  ParseStateType type;
} ParseState;

static ParsedProgramResult shell_parse_input(tstr_view input) {

  // TODO: proper parsing, escapeing, "   " , proper whitespace etc

  ParsedProgram program = {
      .program = {.parsed = tstr_null(), .original = TSTR_TSV("")},
      .args = TVEC_EMPTY(ShellArgument),
  };

  size_t start = 0;
  ParseState state = {.type = ParseStateTypeNormal};
  ParseStateAccumulator acc = {.raw = (tstr_view){.data = input.data, .len = 0},
                               .str = tstr_from("")};

  for (size_t i = 0; i < input.len; ++i) {
    const char character = input.data[i];

    switch (state.type) {
    case ParseStateTypeNormal: {
      if (isspace(character) != 0) {
        if (start == i) {

          // if the accumulator has data, we need to add that data to the
          // arguments, as this is an end delimiter to compound strings ("a'b'c"
          // or similar)
          if (acc.raw.len != 0) {
            ShellArgument argument = {.original = acc.raw, .parsed = acc.str};

            ADD_SHELL_ARGUMENT(argument);

            // we move the tstr out of the accumulator to argument, so no need
            // to free the string
            acc = (ParseStateAccumulator){
                .raw = (tstr_view){.data = input.data + start, .len = 0},
                .str = tstr_from("")};
          }

          // the next chacrater is a whitespace, so skip it, until we either
          // encounter another character as end delimiter
          acc.raw.data += 1;
          acc.raw.len = 0;

          start = i + 1;
        } else {

          const tstr_view orig =
              (tstr_view){.data = input.data + start, .len = i - start};

          const size_t add_raw_span = i - start;

          ADD_TO_ACCUMULATOR(orig, add_raw_span);

          ShellArgument argument = {.original = acc.raw, .parsed = acc.str};

          ADD_SHELL_ARGUMENT(argument);

          start = i + 1;

          // we move the tstr out of the accumulator to argument, so no need to
          // free the string
          acc = (ParseStateAccumulator){
              .raw = (tstr_view){.data = input.data + start, .len = 0},
              .str = tstr_from("")};
        }

      } else if (character == '\'') {
        state = (ParseState){.type = ParseStateTypeSingleQuote};

        if (start == i) {
          // no string before it, just advance start
        } else {
          // if we have some string before ' we need to set the accumulator to
          // it
          // the thing before this is a normal string without quotes, otherwise
          // it would already be consumed
          const tstr_view orig =
              (tstr_view){.data = input.data + start, .len = i - start};
          const size_t add_raw_span = (i - start) + 1;

          ADD_TO_ACCUMULATOR(orig, add_raw_span);
        }
        start = i + 1;

      } else if (character == '"') {
        state = (ParseState){.type = ParseStateTypeDoubleQuote};

        // if we have some string before "" we need to set the accumulator to it
      }

      break;
    }
    case ParseStateTypeSingleQuote: {
      if (character == '\'') {

        const tstr_view orig =
            (tstr_view){.data = input.data + start, .len = i - start};

        const size_t add_raw_span = (i - start) + 2;

        ADD_TO_ACCUMULATOR(orig, add_raw_span);

        state = (ParseState){.type = ParseStateTypeNormal};

        start = i + 1;
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
    const tstr_view orig =
        (tstr_view){.data = input.data + start, .len = input.len - start};

    const size_t add_raw_span = input.len - start;

    ADD_TO_ACCUMULATOR(orig, add_raw_span);

    ShellArgument argument = {.original = acc.raw, .parsed = acc.str};

    ADD_SHELL_ARGUMENT(argument);

    // we move the tstr out of the accumulator to argument, so no need to
    // free the string
    acc = (ParseStateAccumulator){
        .raw = (tstr_view){.data = input.data + start, .len = 0},
        .str = tstr_from("")};
  }

  // drain the accumulator
  if (acc.raw.len != 0) {
    ShellArgument argument = {.original = acc.raw, .parsed = acc.str};

    ADD_SHELL_ARGUMENT(argument);

    // we move the tstr out of the accumulator to argument, so no need
    // to free the string
    acc = (ParseStateAccumulator){
        .raw = (tstr_view){.data = input.data + start, .len = 0},
        .str = tstr_from("")};
  }

  tstr_free(&acc.str);

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
  if (false) {
    int res = fprintf(stderr, "Arguments:\n");
    if (res < 0) {
      return (ShellParseDataResult){.type = ShellParseDataResultTypeError,
                                    .data = {.error = TSTR_LIT("IO Error")}};
    }

    for (size_t i = 0; i < TVEC_LENGTH(ShellArgument, program.args); ++i) {

      const ShellArgument *const arg =
          TVEC_GET_AT(ShellArgument, &(program.args), i);

      int res =
          fprintf(stderr, "<%zu>\n  value: " TSTR_FMT "\n  raw: " TSTR_FMT "\n",
                  i, TSTR_FMT_ARGS(arg->parsed), TSV_FMT_ARGS(arg->original));
      if (res < 0) {
        return (ShellParseDataResult){.type = ShellParseDataResultTypeError,
                                      .data = {.error = TSTR_LIT("IO Error")}};
      }
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
