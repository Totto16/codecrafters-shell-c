#include "./builtins.h"
#include "./executable.h"

#define RETURN_ARGUMENT(Type, value)                                           \
  do {                                                                         \
    Type *data = malloc(sizeof(Type));                                         \
    if (data == NULL) {                                                        \
      return (ShellParseArgumentsResult){                                      \
          .is_ok = false,                                                      \
          .data = {.error = TSTR_LIT("OOM in argument allocation")}};          \
    }                                                                          \
    *data = value;                                                             \
    return (ShellParseArgumentsResult){.is_ok = true, .data = {.args = data}}; \
  } while (false)

static ShellParseArgumentsResult
exit_parse_arguments(const ShellArguments *args) {
  if (args->length == 0) {
    ExitData exit_data = {.has_code = false, .code = ExitCodeSuccess};
    RETURN_ARGUMENT(ExitData, exit_data);
  }

  if (args->length == 1) {
    const ShellArgument *const arg1 = TVEC_GET_AT(ShellArgument, args, 0);
    int result = 0;
    bool ok = tstr_view_to_int(tstr_as_view(&(arg1->parsed)), &result);

    if (!ok) {
      return (ShellParseArgumentsResult){
          .is_ok = false,
          .data = {.error = TSTR_LIT(
                       "Invalid argument for exit: [n]\nNot a number")}};
    }

    if (result < 0 || result > UINT8_MAX) {
      return (ShellParseArgumentsResult){
          .is_ok = false,
          .data = {.error = TSTR_LIT(
                       "Invalid argument for exit: [n]\nNumber out of range")}};
    }
    ExitData exit_data = {.has_code = true, .code = (uint8_t)result};
    RETURN_ARGUMENT(ExitData, exit_data);
  }

  return (ShellParseArgumentsResult){
      .is_ok = false, .data = {.error = TSTR_LIT("exit: too many arguments")}};
}

static void exit_free_arguments(shell_dynamic_args args) { free(args); }

ExitCode exit_invoke(shell_dynamic_args args) {
  (void)args;
  return ExitCodeImplementationError;
}

static tstr_static exit_help(void) {
  return TSTR_STATIC_LIT(
      "exit: exit [n]\n"
      "Exit the shell.\n"
      "\n"
      "Exits the shell with a status of N.  If N is omitted, the exit status\n"
      "is that of the last command executed.\n");
}

const static BuiltinCommand builtin_function_exit = {
    .name = TSTR_STATIC_LIT("exit"),
    .validator = {.parse = exit_parse_arguments, .free = exit_free_arguments},
    .invoke = exit_invoke,
    .help = exit_help,
};

typedef struct {
  tstr_view original;
  tstr parsed;
} EchoArgument;

TVEC_DEFINE_AND_IMPLEMENT_VEC_TYPE(EchoArgument)

typedef TVEC_TYPENAME(EchoArgument) EchoArguments;

typedef struct {
  bool newline;
  bool escapes;
  EchoArguments args;
} EchoData;

static ShellParseArgumentsResult
echo_parse_arguments(const ShellArguments *args) {

  bool processed_args = false;

  EchoData echo_data = {
      .newline = true,
      .escapes = false,
      .args = TVEC_EMPTY(EchoArgument),
  };

  for (size_t i = 0; i < TVEC_LENGTH(ShellArgument, *args); ++i) {

    const ShellArgument *const arg = TVEC_GET_AT(ShellArgument, args, i);

    if (!processed_args) {

      // TODO: support also -enE etc.

      if (tstr_eq_static_tstr(&(arg->parsed), TSTR_STATIC_LIT("-n"))) {
        echo_data.newline = false;
        continue;
      } else if (tstr_eq_static_tstr(&(arg->parsed), TSTR_STATIC_LIT("-e"))) {
        echo_data.escapes = true;
        continue;
      } else if (tstr_eq_static_tstr(&(arg->parsed), TSTR_STATIC_LIT("-E"))) {
        echo_data.escapes = false;
        continue;
      }

      processed_args = true;
    }

    // TODO: process escapes, if there are any and it is enabled

    EchoArgument argument = {.original = tstr_as_view(&(arg->parsed)),
                             .parsed = tstr_dup(&(arg->parsed))};

    TvecResult res = TVEC_PUSH(EchoArgument, &(echo_data.args), argument);

    if (res != TvecResultOk) { // NOLINT(readability-implicit-bool-conversion)
      return (ShellParseArgumentsResult){
          .is_ok = false,
          .data = {.error = TSTR_LIT("OOM in argument allocation")}};
    }
  }

  RETURN_ARGUMENT(EchoData, echo_data);
}

static void free_echo_argument(EchoArgument *arg) { tstr_free(&(arg->parsed)); }

static void free_echo_arguments(EchoArguments *args) {

  for (size_t i = 0; i < TVEC_LENGTH(EchoArgument, *args); ++i) {

    EchoArgument *arg = TVEC_GET_AT_MUT(EchoArgument, args, i);

    free_echo_argument(arg);
  }

  TVEC_FREE(EchoArgument, args);
}

static void echo_free_arguments(shell_dynamic_args args) {

  EchoData *echo_data = args;

  free_echo_arguments(&(echo_data->args));

  free(args);
}

static ExitCode echo_invoke(shell_dynamic_args args) {

  const EchoData *const echo_data = args;

  for (size_t i = 0; i < TVEC_LENGTH(EchoArgument, echo_data->args); ++i) {

    if (i != 0) {
      int res = fprintf(stdout, " ");
      if (res < 0) {
        return ExitCodeGenericError;
      }
    }

    const EchoArgument *const arg =
        TVEC_GET_AT(EchoArgument, &(echo_data->args), i);

    int res = fprintf(stdout, TSTR_FMT, TSTR_FMT_ARGS(arg->parsed));
    if (res < 0) {
      return ExitCodeGenericError;
    }
  }

  if (echo_data->newline) {
    int res = fprintf(stdout, "\n");
    if (res < 0) {
      return ExitCodeGenericError;
    }
  }

  return ExitCodeSuccess;
}

static tstr_static echo_help(void) {
  return TSTR_STATIC_LIT(
      "echo: echo [-neE] [arg ...]\n"
      "Write arguments to the standard output."
      "\n"
      "Display the ARGs, separated by a single space character and followed by "
      "a\n"
      "newline, on the standard output.\n"
      "\n"
      "Options:\n"
      "  -n        do not append a newline\n"
      "  -e        enable interpretation of the following backslash escapes\n"
      "  -E        explicitly suppress interpretation of backslash escapes\n"
      "\n"
      "`echo' interprets the following backslash-escaped characters:\n"
      "  \\a        alert (bell)\n"
      "  \\b        backspace\n"
      "  \\c        suppress further output\n"
      "  \\e        escape character\n"
      "  \\E        escape character\n"
      "  \\f        form feed\n"
      "  \\n        new line\n"
      "  \\r        carriage return\n"
      "  \\t        horizontal tab\n"
      "  \\v        vertical tab\n"
      "  \\\\        backslash\n"
      "  \\0nnn     the character whose ASCII code is NNN (octal).  NNN can "
      "be\n"
      "            0 to 3 octal digits\n"
      "  \\xHH      the eight-bit character whose value is HH (hexadecimal).  "
      "HH\n"
      "            can be one or two hex digits\n"
      "  \\uHHHH    the Unicode character whose value is the hexadecimal value "
      "HHHH.\n"
      "            HHHH can be one to four hex digits.\n"
      "  \\UHHHHHHHH the Unicode character whose value is the hexadecimal "
      "value\n"
      "            HHHHHHHH. HHHHHHHH can be one to eight hex digits.\n"
      "\n"
      "Exit Status:\n"
      "Returns success unless a write error occurs.\n");
}

const static BuiltinCommand builtin_function_echo = {
    .name = TSTR_STATIC_LIT("echo"),
    .validator = {.parse = echo_parse_arguments, .free = echo_free_arguments},
    .invoke = echo_invoke,
    .help = echo_help,
};

typedef struct {
  tstr_view name;
} TypeArgument;

TVEC_DEFINE_AND_IMPLEMENT_VEC_TYPE(TypeArgument)

typedef TVEC_TYPENAME(TypeArgument) TypeArguments;

typedef struct {
  TypeArguments args;
} TypeData;

static ShellParseArgumentsResult
type_parse_arguments(const ShellArguments *args) {

  if (args->length == 0) {
    return (ShellParseArgumentsResult){
        .is_ok = false,
        .data = {.error = TSTR_LIT("type: expected at least one argument")}};
  }

  TypeData type_data = {
      .args = TVEC_EMPTY(TypeArgument),
  };

  // TODO: support options

  for (size_t i = 0; i < TVEC_LENGTH(ShellArgument, *args); ++i) {

    const ShellArgument *const arg = TVEC_GET_AT(ShellArgument, args, i);

    TypeArgument argument = {.name = tstr_as_view(&(arg->parsed))};

    TvecResult res = TVEC_PUSH(TypeArgument, &(type_data.args), argument);

    if (res != TvecResultOk) { // NOLINT(readability-implicit-bool-conversion)
      return (ShellParseArgumentsResult){
          .is_ok = false,
          .data = {.error = TSTR_LIT("OOM in argument allocation")}};
    }
  }

  RETURN_ARGUMENT(TypeData, type_data);
}

static void free_type_arguments(TypeArguments *args) {

  TVEC_FREE(TypeArgument, args);
}

static void type_free_arguments(shell_dynamic_args args) {
  TypeData *type_data = args;

  free_type_arguments(&(type_data->args));

  free(args);
}

typedef enum : uint8_t {
  CommandTypeTypeBuiltin = 0,
  CommandTypeTypeExecutable,
  CommandTypeTypeNotFound,
} CommandTypeType;

typedef struct {
  tstr path;
} ExecutableData;

static void free_executable_data(ExecutableData *data) {
  tstr_free(&(data->path));
}

typedef struct {
  CommandTypeType type;
  union {
    ExecutableData executable;
  } data;
} CommandType;

static CommandType determine_command_type(tstr_view command) {
  for (size_t i = 0; i < BUILTIN_FUNCTION_COUNT; ++i) {

    BuiltinCommand builtin_command = builtin_functions[i];

    if (tstr_view_eq_view(command, tstr_static_as_view(builtin_command.name))) {

      return (CommandType){.type = CommandTypeTypeBuiltin, .data = {}};
    }
  }

  tstr executable_path = get_executable_path(command);

  if (!tstr_is_null(&executable_path)) {
    return (CommandType){.type = CommandTypeTypeExecutable,
                         .data = {.executable = {.path = executable_path}}};
  }

  return (CommandType){.type = CommandTypeTypeNotFound, .data = {}};
}

static ExitCode type_invoke(shell_dynamic_args args) {

  const TypeData *const type_data = args;

  ExitCode code = ExitCodeSuccess;

  for (size_t i = 0; i < TVEC_LENGTH(TypeArgument, type_data->args); ++i) {

    const TypeArgument *const arg =
        TVEC_GET_AT(TypeArgument, &(type_data->args), i);

    CommandType command_type = determine_command_type(arg->name);

    switch (command_type.type) {
    case CommandTypeTypeBuiltin: {
      int res = fprintf(stdout, TSTR_FMT " is a shell builtin\n",
                        TSV_FMT_ARGS(arg->name));
      if (res < 0) {
        return ExitCodeGenericError;
      }
      break;
    }
    case CommandTypeTypeExecutable: {
      int res = fprintf(stdout, TSTR_FMT " is " TSTR_FMT "\n",
                        TSV_FMT_ARGS(arg->name),
                        TSTR_FMT_ARGS(command_type.data.executable.path));

      free_executable_data(&command_type.data.executable);

      if (res < 0) {
        return ExitCodeGenericError;
      }
      break;
    }
    case CommandTypeTypeNotFound: {
      code = ExitCodeGenericError;
      int res =
          fprintf(stdout, TSTR_FMT ": not found\n", TSV_FMT_ARGS(arg->name));
      if (res < 0) {
        return ExitCodeGenericError;
      }
      break;
    }
    default: {
      return ExitCodeImplementationError;
    }
    }
  }

  return code;
}

static tstr_static type_help(void) {
  return TSTR_STATIC_LIT(
      "type: type [-afptP] name [name ...]\n"
      "Display information about command type.\n"
      "\n"
      "For each NAME, indicate how it would be interpreted if used as a\n"
      "command name.\n"
      "\n"
      "Options:\n"
      "  -a        display all locations containing an executable named NAME;\n"
      "            includes aliases, builtins, and functions, if and only if\n"
      "            the `-p' option is not also used\n"
      "  -f        suppress shell function lookup\n"
      "  -P        force a PATH search for each NAME, even if it is an alias,\n"
      "            builtin, or function, and returns the name of the disk "
      "file\n"
      "            that would be executed\n"
      "  -p        returns either the name of the disk file that would be "
      "executed,\n"
      "            or nothing if `type -t NAME' would not return `file'\n"
      "  -t        output a single word which is one of `alias', `keyword',\n"
      "            `function', `builtin', `file' or `', if NAME is an alias,\n"
      "            shell reserved word, shell function, shell builtin, disk "
      "file,\n"
      "            or not found, respectively\n"
      "\n"
      "Arguments:\n"
      "  NAME      Command name to be interpreted.\n"
      "\n"
      "Exit Status:\n"
      "Returns success if all of the NAMEs are found; fails if any are not "
      "found.\n");
}

const static BuiltinCommand builtin_function_type = {
    .name = TSTR_STATIC_LIT("type"),
    .validator = {.parse = type_parse_arguments, .free = type_free_arguments},
    .invoke = type_invoke,
    .help = type_help,
};

BuiltinCommand builtin_functions[] = {
    builtin_function_exit,
    builtin_function_echo,
    builtin_function_type,
    // TODO: help
};

static_assert(sizeof(builtin_functions) / sizeof(builtin_functions[0]) ==
                  BUILTIN_FUNCTION_COUNT,
              "BUILTIN_FUNCTION_COUNT is incorrect");
