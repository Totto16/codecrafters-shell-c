
#include "./parse.h"
#include "./command.h"
#include "./file.h"

typedef struct {
  ShellArgument program;
  ShellArguments args;
  ShellRedirectSettings settings;
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
  ParseStateTypeEscape,
} ParseStateType;

typedef struct {
  tstr str;
  tstr_view raw;
} ParseStateAccumulator;

typedef enum : uint8_t {
  ParseStateDoubleQuoteTypeNormal = 0,
  ParseStateDoubleQuoteTypeEscape,
} ParseStateDoubleQuoteType;

typedef struct {
  ParseStateDoubleQuoteType type;
} ParseStateDoubleQuote;

typedef struct {
  ParseStateType type;
  union {
    ParseStateDoubleQuote double_quote;
  } data;
} ParseState;

#define RETURN_IMPL_ERROR(err)                                                 \
  do {                                                                         \
    return (ParsedProgramResult){.is_ok = false,                               \
                                 .data = {.error = TSTR_LIT(err)}};            \
                                                                               \
  } while (false)

static void free_shell_argument(ShellArgument *arg);

static ParsedProgramResult shell_parse_input(tstr_view input,
                                             const ShellHandle shell) {
  ParsedProgram program = {
      .program = {.parsed = tstr_null(), .original = TSTR_TSV("")},
      .args = TVEC_EMPTY(ShellArgument),
      .settings = {.stdout = {.location = tstr_null(), .append = false},
                   .stderr = {.location = tstr_null(), .append = false}}};

  size_t start = 0;
  ParseState state = {.type = ParseStateTypeNormal, .data = {}};
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
        state = (ParseState){.type = ParseStateTypeSingleQuote, .data = {}};

        if (start == i) {
          // no string before it, just advance start
        } else {
          // if we have some string before ' we need to set the accumulator
          // to it the thing before this is a normal string without quotes,
          // otherwise it would already be consumed
          const tstr_view orig =
              (tstr_view){.data = input.data + start, .len = i - start};
          const size_t add_raw_span = (i - start) + 1;

          ADD_TO_ACCUMULATOR(orig, add_raw_span);
        }
        start = i + 1;
      } else if (character == '"') {
        state = (ParseState){
            .type = ParseStateTypeDoubleQuote,
            .data = {
                .double_quote = {.type = ParseStateDoubleQuoteTypeNormal}}};

        if (start == i) {
          // no string before it, just advance start
        } else {
          // if we have some string before " we need to set the accumulator
          // to it the thing before this is a normal string without quotes,
          // otherwise it would already be consumed
          const tstr_view orig =
              (tstr_view){.data = input.data + start, .len = i - start};
          const size_t add_raw_span = (i - start) + 1;

          ADD_TO_ACCUMULATOR(orig, add_raw_span);
        }
        start = i + 1;
      } else if (character == '\\') {
        state = (ParseState){.type = ParseStateTypeEscape, .data = {}};

        if (start == i) {
          // no string before it, just advance start
        } else {
          // if we have some string before \ we need to set the accumulator
          // to it the thing before this is a normal string without quotes,
          // otherwise it would already be consumed
          const tstr_view orig =
              (tstr_view){.data = input.data + start, .len = i - start};
          const size_t add_raw_span = (i - start) + 1;

          ADD_TO_ACCUMULATOR(orig, add_raw_span);
        }
        start = i + 1;
      }

      break;
    }
    case ParseStateTypeSingleQuote: {
      if (character == '\'') {

        const tstr_view orig =
            (tstr_view){.data = input.data + start, .len = i - start};

        const size_t add_raw_span = (i - start) + 2;

        ADD_TO_ACCUMULATOR(orig, add_raw_span);

        state = (ParseState){.type = ParseStateTypeNormal, .data = {}};

        start = i + 1;
      }
      break;
    }
    case ParseStateTypeDoubleQuote: {
      switch (state.data.double_quote.type) {
      case ParseStateDoubleQuoteTypeNormal: {
        if (character == '"') {
          const tstr_view orig = (tstr_view){.data = input.data + i, .len = 0};

          const size_t add_raw_span = 1;

          ADD_TO_ACCUMULATOR(orig, add_raw_span);

          state = (ParseState){.type = ParseStateTypeNormal, .data = {}};

          start = i + 1;
        } else if (character == '\\') {
          state.data.double_quote =
              (ParseStateDoubleQuote){.type = ParseStateDoubleQuoteTypeEscape};
        } else {
          const tstr_view orig = (tstr_view){.data = input.data + i, .len = 1};

          const size_t add_raw_span = 1;

          ADD_TO_ACCUMULATOR(orig, add_raw_span);
        }
        break;
      }
      case ParseStateDoubleQuoteTypeEscape: {

        if (character == '"' || character == '\\' || character == '$' ||
            character == '`' || character == '\n') {
          const tstr_view orig = (tstr_view){.data = input.data + i, .len = 1};

          const size_t add_raw_span = 2;

          ADD_TO_ACCUMULATOR(orig, add_raw_span);

        } else {
          const tstr_view orig =
              (tstr_view){.data = input.data + i - 1, .len = 2};

          const size_t add_raw_span = 2;

          ADD_TO_ACCUMULATOR(orig, add_raw_span);
        }

        state.data.double_quote =
            (ParseStateDoubleQuote){.type = ParseStateDoubleQuoteTypeNormal};

        break;
      }
      default: {
        return (ParsedProgramResult){
            .is_ok = false,
            .data = {.error = TSTR_LIT(
                         "[INTERNAL] wrong double quote parse state")}};
      }
      }
      break;
    }
    case ParseStateTypeEscape: {

      const tstr_view orig = (tstr_view){.data = input.data + i, .len = 1};

      const size_t add_raw_span = 1;

      ADD_TO_ACCUMULATOR(orig, add_raw_span);

      state = (ParseState){.type = ParseStateTypeNormal, .data = {}};

      start = i + 1;

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
    } else if (state.type == ParseStateTypeEscape) {
      return (ParsedProgramResult){
          .is_ok = false,
          .data = {.error = TSTR_LIT("Unterminated escape sequence")}};
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

  // check for redirect settings, this is easier to do here, as it would get
  // complicated in the parse step

  for (size_t i = 0; i < TVEC_LENGTH(ShellArgument, program.args); ++i) {

    ShellArgument *const arg =
        TVEC_GET_AT_MUT(ShellArgument, &(program.args), i);

    // check for supported redirections, use the original string, as quoted
    // strings are not valid
    ShellRedirectLocation *location = NULL;
    bool append = false;

    if (tstr_view_eq(arg->original, ">") || tstr_view_eq(arg->original, "1>")) {
      location = &program.settings.stdout;
      append = false;
    } else if (tstr_view_eq(arg->original, "2>")) {
      location = &program.settings.stderr;
      append = false;
    } else if (tstr_view_eq(arg->original, ">>") ||
               tstr_view_eq(arg->original, "1>>")) {
      location = &program.settings.stdout;
      append = true;
    } else if (tstr_view_eq(arg->original, "2>>")) {
      location = &program.settings.stderr;
      append = true;
    }

    if (location != NULL) {

      // check that we only have one redirection per program per output stream
      if (!tstr_is_null(&(location->location))) {
        return (ParsedProgramResult){
            .is_ok = false,
            .data = {
                .error = TSTR_LIT(
                    "more than one of the same redirection not supported")}};
      }

      // check that we have an argument
      if (i >= TVEC_LENGTH(ShellArgument, program.args) - 1) {
        return (ParsedProgramResult){
            .is_ok = false,
            .data = {.error = TSTR_LIT("Unterminated redirection")}};
      }

      ShellArgument *const next_arg =
          TVEC_GET_AT_MUT(ShellArgument, &(program.args), i + 1);

      // resolve it to an absolute path, set the location
      {
        tstr_view cwd =
            shell.function.get_current_working_directory(shell.context);

        if (cwd.len == 0) {
          RETURN_IMPL_ERROR("cwd get error");
        }

        tstr resolved_path =
            path_resolve_to_absolute(cwd, tstr_as_view(&(next_arg->parsed)));

        if (tstr_is_null(&resolved_path)) {
          RETURN_IMPL_ERROR("path resolve error");
        }

        if (!path_is_absolute(tstr_as_view(&resolved_path))) {
          tstr_free(&resolved_path);
          RETURN_IMPL_ERROR("resolved path is not absolute");
        }

        tstr path = path_normalize(tstr_as_view(&resolved_path));

        tstr_free(&resolved_path);

        if (tstr_is_null(&path)) {
          RETURN_IMPL_ERROR("path normalize error");
        }

        if (!path_is_absolute(tstr_as_view(&path))) {
          tstr_free(&path);
          RETURN_IMPL_ERROR("normalized path is not absolute");
        }

        *location = (ShellRedirectLocation){.location = path, .append = append};
      }

      // NOTE: remove those two value from the array, we are iterating over it,
      //  so this has to be done correctly

      free_shell_argument(arg);
      free_shell_argument(next_arg);

      TVEC_REMOVE(ShellArgument, &(program.args), i + 1);
      TVEC_REMOVE(ShellArgument, &(program.args), i);

      // NOTE: we can do --i, even if we underflow, as we overflow right into it
      // again
      --i;
    }
  }

  return (ParsedProgramResult){.is_ok = true, .data = {.program = program}};
}

static void free_shell_argument(ShellArgument *arg) {
  tstr_free(&(arg->parsed));
}

static void free_shell_redirect_location(ShellRedirectLocation *location) {

  tstr_free(&(location->location));
}

static void free_shell_redirect_settings(ShellRedirectSettings *settings) {
  free_shell_redirect_location(&(settings->stdout));
  free_shell_redirect_location(&(settings->stderr));
}

static void free_shell_arguments(ShellArguments *args) {

  for (size_t i = 0; i < TVEC_LENGTH(ShellArgument, *args); ++i) {

    ShellArgument *arg = TVEC_GET_AT_MUT(ShellArgument, args, i);

    free_shell_argument(arg);
  }

  TVEC_FREE(ShellArgument, args);
}

static void free_parsed_program(ParsedProgram *program) {
  free_shell_arguments(&(program->args));
  free_shell_argument(&(program->program));
  free_shell_redirect_settings(&(program->settings));
}

void free_shell_command(ShellCommand *command) {
  free_shell_arguments(&(command->args));
  free_shell_redirect_settings(&(command->settings));
}

void free_shell_command_not_found(ShellCommandNotFound *found) {
  tstr_free(&(found->name));
}

ShellParseDataResult shell_parse_data(tstr_view input,
                                      const ShellHandle shell) {

  ParsedProgramResult program_res = shell_parse_input(input, shell);

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
                             .args = program.args,
                             .settings = program.settings}}};
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
                             .args = program.args,
                             .settings = program.settings}}};
  }
  default: {
    // TODO: report internal error
    break;
  }
  }

  free_shell_arguments(&program.args);
  free_shell_redirect_settings(&program.settings);

  return (ShellParseDataResult){
      .type = ShellParseDataResultTypeCommandNotFound,
      .data = {.command_not_found = {.name = program.program.parsed}}};
}
