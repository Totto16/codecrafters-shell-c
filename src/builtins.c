#include "./builtins.h"
#include "./command.h"
#include "./file.h"
#include "./utils.h"

#include <errno.h>
#include <fcntl.h>
#include <unistd.h>

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

static ExitCode exit_invoke(shell_dynamic_args args, const ShellHandle shell,
                            const ShellIO io) {
  UNUSED(io);
  const ExitData *const exit_data = args;

  shell.function.exit(shell.context, exit_data);
  return ExitCodeSuccess;
}

static tstr_static exit_help(void) {
  return TSTR_STATIC_LIT(
      "exit: exit [n]\n"
      "Exit the shell.\n"
      "\n"
      "Exits the shell with a status of N.  If N is omitted, the exit status\n"
      "is that of the last command executed.\n");
}

static const BuiltinCommand builtin_function_exit = {
    .name = TSTR_STATIC_LIT_CONST("exit"),
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

#define SHELL_FPINTF(...) io.function.fprintf(io.context, __VA_ARGS__)

#define RETURN_IO_ERROR(name)                                                  \
  do {                                                                         \
    int res = SHELL_FPINTF(stderr, "%s: IO Error\n", name);                    \
    if (res < 0) {                                                             \
      /* Note: we already have an error, so no reporting possible here */      \
    }                                                                          \
    return ExitCodeIOError;                                                    \
  } while (false)

#define RETURN_IMPL_ERROR(name, msg)                                           \
  do {                                                                         \
    int res = SHELL_FPINTF(                                                    \
        stderr, "%s: [INTERNAL] Implementation Error: %s\n", name, msg);       \
    if (res < 0) {                                                             \
      RETURN_IO_ERROR(name);                                                   \
    }                                                                          \
    return ExitCodeImplementationError;                                        \
  } while (false)

static ExitCode echo_invoke(shell_dynamic_args args, const ShellHandle shell,
                            const ShellIO io) {
  UNUSED(shell);
  const EchoData *const echo_data = args;

  for (size_t i = 0; i < TVEC_LENGTH(EchoArgument, echo_data->args); ++i) {

    if (i != 0) {
      int res = SHELL_FPINTF(stdout, " ");
      if (res < 0) {
        RETURN_IO_ERROR("echo");
      }
    }

    const EchoArgument *const arg =
        TVEC_GET_AT(EchoArgument, &(echo_data->args), i);

    int res = SHELL_FPINTF(stdout, TSTR_FMT, TSTR_FMT_ARGS(arg->parsed));
    if (res < 0) {
      RETURN_IO_ERROR("echo");
    }
  }

  if (echo_data->newline) {
    int res = SHELL_FPINTF(stdout, "\n");
    if (res < 0) {
      RETURN_IO_ERROR("echo");
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

static const BuiltinCommand builtin_function_echo = {
    .name = TSTR_STATIC_LIT_CONST("echo"),
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

static ExitCode type_invoke(shell_dynamic_args args, const ShellHandle shell,
                            const ShellIO io) {
  UNUSED(shell);
  const TypeData *const type_data = args;

  ExitCode code = ExitCodeSuccess;

  for (size_t i = 0; i < TVEC_LENGTH(TypeArgument, type_data->args); ++i) {

    const TypeArgument *const arg =
        TVEC_GET_AT(TypeArgument, &(type_data->args), i);

    CommandType command_type = determine_command_type(arg->name);

    switch (command_type.type) {
    case CommandTypeTypeBuiltin: {
      int res = SHELL_FPINTF(stdout, TSTR_FMT " is a shell builtin\n",
                             TSV_FMT_ARGS(arg->name));
      if (res < 0) {
        RETURN_IO_ERROR("type");
      }
      break;
    }
    case CommandTypeTypeExecutable: {
      int res = SHELL_FPINTF(stdout, TSTR_FMT " is " TSTR_FMT "\n",
                             TSV_FMT_ARGS(arg->name),
                             TSTR_FMT_ARGS(command_type.data.executable.path));

      free_executable_data(&command_type.data.executable);

      if (res < 0) {
        RETURN_IO_ERROR("type");
      }
      break;
    }
    case CommandTypeTypeNotFound: {
      code = ExitCodeGenericError;
      int res = SHELL_FPINTF(stdout, TSTR_FMT ": not found\n",
                             TSV_FMT_ARGS(arg->name));
      if (res < 0) {
        RETURN_IO_ERROR("type");
      }
      break;
    }
    default: {
      RETURN_IMPL_ERROR("type", "Invalid command type");
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

static const BuiltinCommand builtin_function_type = {
    .name = TSTR_STATIC_LIT_CONST("type"),
    .validator = {.parse = type_parse_arguments, .free = type_free_arguments},
    .invoke = type_invoke,
    .help = type_help,
};

typedef struct {
  bool _unused : 1;
} Monostate;

#define MONOSTATE ((Monostate){._unused = 0})

typedef struct {
  Monostate _unused;
} PwdData;

static ShellParseArgumentsResult
pwd_parse_arguments(const ShellArguments *args) {

  // TODO: support options to pwd

  if (args->length == 0) {
    PwdData pwd_data = {._unused = MONOSTATE};
    RETURN_ARGUMENT(PwdData, pwd_data);
  }
  return (ShellParseArgumentsResult){
      .is_ok = false, .data = {.error = TSTR_LIT("pwd: too many arguments")}};
}

static void pwd_free_arguments(shell_dynamic_args args) { free(args); }

static ExitCode pwd_invoke(shell_dynamic_args args, const ShellHandle shell,
                           const ShellIO io) {
  UNUSED(args);

  tstr_view cwd = shell.function.get_current_working_directory(shell.context);

  if (cwd.len == 0) {
    RETURN_IMPL_ERROR("pwd", "cwd get error");
  }

  int res = SHELL_FPINTF(stdout, TSTR_FMT "\n", TSV_FMT_ARGS(cwd));

  if (res < 0) {
    RETURN_IO_ERROR("pwd");
  }

  return ExitCodeSuccess;
}

static tstr_static pwd_help(void) {
  return TSTR_STATIC_LIT(
      "pwd: pwd [-LP]\n"
      "Print the name of the current working directory.\n"
      "\n"
      "Options:\n"
      "  -L        print the value of $PWD if it names the current working\n"
      "            directory\n"
      "  -P        print the physical directory, without any symbolic links\n"
      "\n"
      "By default, `pwd' behaves as if `-L' were specified.\n"
      "\n"
      "Exit Status:\n"
      "Returns 0 unless an invalid option is given or the current directory\n"
      "cannot be read.\n");
}

static const BuiltinCommand builtin_function_pwd = {
    .name = TSTR_STATIC_LIT_CONST("pwd"),
    .validator = {.parse = pwd_parse_arguments, .free = pwd_free_arguments},
    .invoke = pwd_invoke,
    .help = pwd_help,
};

typedef struct {
  tstr_view path;
} CdData;

static ShellParseArgumentsResult
cd_parse_arguments(const ShellArguments *args) {

  // TODO: support options to cd

  if (args->length == 0) {
    // TODO: support this
    return (ShellParseArgumentsResult){
        .is_ok = false, .data = {.error = TSTR_LIT("cd: not implemented yet")}};
  } else if (args->length == 1) {
    const ShellArgument *const arg1 = TVEC_GET_AT(ShellArgument, args, 0);

    CdData cd_data = {.path = tstr_as_view(&(arg1->parsed))};
    RETURN_ARGUMENT(CdData, cd_data);
  }

  return (ShellParseArgumentsResult){
      .is_ok = false, .data = {.error = TSTR_LIT("cd: too many arguments")}};
}

static void cd_free_arguments(shell_dynamic_args args) { free(args); }

static ExitCode cd_invoke(shell_dynamic_args args, const ShellHandle shell,
                          const ShellIO io) {
  CdData *cd_data = args;

  tstr_view cwd = shell.function.get_current_working_directory(shell.context);

  if (cwd.len == 0) {
    RETURN_IMPL_ERROR("cd", "cwd get error");
  }

  tstr resolved_path = path_resolve_to_absolute(cwd, cd_data->path);

  if (tstr_is_null(&resolved_path)) {
    RETURN_IMPL_ERROR("cd", "path resolve error");
  }

  if (!path_is_absolute(tstr_as_view(&resolved_path))) {
    tstr_free(&resolved_path);
    RETURN_IMPL_ERROR("cd", "resolved path is not absolute");
  }

  tstr path = path_normalize(tstr_as_view(&resolved_path));

  tstr_free(&resolved_path);

  if (tstr_is_null(&path)) {
    RETURN_IMPL_ERROR("cd", "path normalize error");
  }

  if (!path_is_absolute(tstr_as_view(&path))) {
    tstr_free(&path);
    RETURN_IMPL_ERROR("cd", "normalized path is not absolute");
  }

  if (!path_exists_and_is_dir(tstr_as_view(&path))) {
    int res =
        SHELL_FPINTF(stdout, "cd: " TSTR_FMT ": No such file or directory\n",
                     TSV_FMT_ARGS(cd_data->path));
    if (res < 0) {
      RETURN_IO_ERROR("cd");
    }

    return ExitCodeGenericError;
  }

  bool ok = shell.function.set_current_working_directory(shell.context, &path);

  tstr_free(&path);

  if (!ok) {
    RETURN_IMPL_ERROR("cd", "cwd set error");
  }

  return ExitCodeSuccess;
}

static tstr_static cd_help(void) {
  return TSTR_STATIC_LIT(
      "cd: cd [-L|[-P [-e]]] [-@] [dir]\n"
      "Change the shell working directory.\n"
      "\n"
      "Change the current directory to DIR.  The default DIR is the value of "
      "the\n"
      "HOME shell variable. If DIR is \" -\", it is converted to $OLDPWD.\n"
      "\n"
      "The variable CDPATH defines the search path for the directory "
      "containing\n"
      "DIR.  Alternative directory names in CDPATH are separated by a colon "
      "(:).\n"
      "A null directory name is the same as the current directory.  If DIR "
      "begins\n"
      "with a slash (/), then CDPATH is not used.\n"
      "\n"
      "If the directory is not found, and the shell option `cdable_vars' is "
      "set,\n"
      "the word is assumed to be  a variable name.  If that variable has a "
      "value,\n"
      "its value is used for DIR.\n"
      "\n"
      "Options:\n"
      "  -L        force symbolic links to be followed: resolve symbolic\n"
      "            links in DIR after processing instances of `..'\n"
      "  -P        use the physical directory structure without following\n"
      "            symbolic links: resolve symbolic links in DIR before\n"
      "            processing instances of `..'\n"
      "  -e        if the -P option is supplied, and the current working\n"
      "            directory cannot be determined successfully, exit with\n"
      "            a non-zero status\n"
      "  -@        on systems that support it, present a file with extended\n"
      "            attributes as a directory containing the file attributes\n"
      "\n"
      "The default is to follow symbolic links, as if `-L' were specified.\n"
      "`..' is processed by removing the immediately previous pathname "
      "component\n"
      "back to a slash or the beginning of DIR.\n"
      "\n"
      "Exit Status:\n"
      "Returns 0 if the directory is changed, and if $PWD is set successfully "
      "when\n"
      "-P is used; non-zero otherwise.\n");
}

static const BuiltinCommand builtin_function_cd = {
    .name = TSTR_STATIC_LIT_CONST("cd"),
    .validator = {.parse = cd_parse_arguments, .free = cd_free_arguments},
    .invoke = cd_invoke,
    .help = cd_help,
};

BuiltinCommand g_builtin_functions[] = {
    builtin_function_exit, builtin_function_echo, builtin_function_type,
    builtin_function_pwd,  builtin_function_cd,
    // TODO: add help builtin
};

static_assert(ARRAY_SIZE(g_builtin_functions) == BUILTIN_FUNCTION_COUNT,
              "BUILTIN_FUNCTION_COUNT is incorrect");

typedef enum : uint8_t {
  ShellIOContextStateTypeNormal = 0,
  ShellIOContextStateTypeRedirect,
} ShellIOContextStateType;

typedef struct {
  int fd;
} ShellIOContextStateRedirect;

typedef struct {
  ShellIOContextStateType type;
  union {
    ShellIOContextStateRedirect redirect;
  } data;
} ShellIOContextState;

typedef struct {
  ShellIOContextState stdout;
  ShellIOContextState stderr;
} ShellIOContext;

static int shell_io_fn_fprintf(shell_io_context ctx, FILE *restrict stream,
                               const char *restrict format, ...) {
  va_list args;
  va_start(args, format);
  int ret;

  ShellIOContext *io_ctx = ctx;

  if (stream == stdout) {
    if (io_ctx->stdout.type == ShellIOContextStateTypeNormal) {
      ret = vfprintf(stream, format, args);
    } else if (io_ctx->stdout.type == ShellIOContextStateTypeRedirect) {
      ret = vdprintf(io_ctx->stdout.data.redirect.fd, format, args);
    } else {
      errno = EBADF;
      ret = -1;
    }
  } else if (stream == stderr) {
    if (io_ctx->stderr.type == ShellIOContextStateTypeNormal) {
      ret = vfprintf(stream, format, args);
    } else if (io_ctx->stderr.type == ShellIOContextStateTypeRedirect) {
      ret = vdprintf(io_ctx->stderr.data.redirect.fd, format, args);
    } else {
      errno = EBADF;
      ret = -1;
    }
  } else {

    errno = ENOSTR;
    ret = -1;
  }

  va_end(args);
  return ret;
}

static void free_shell_io_context_state(ShellIOContextState state) {
  if (state.type == ShellIOContextStateTypeRedirect) {
    shell_redirect_close_file(state.data.redirect.fd);
  }
}

static void free_shell_io_context(ShellIOContext *context) {

  free_shell_io_context_state(context->stdout);
  free_shell_io_context_state(context->stderr);
  free(context);
}

static ShellIO
get_shell_io_in_current_context(const ShellRedirectSettings *const settings) {

  ShellIOContext *context = malloc(sizeof(ShellIOContext));

  if (context == NULL) {
    return (ShellIO){.context = NULL, .function = {.fprintf = NULL}};
  }

  *context = (ShellIOContext){
      .stdout = {.type = ShellIOContextStateTypeNormal, .data = {}},
      .stderr = {.type = ShellIOContextStateTypeNormal, .data = {}},
  };

  if (shell_redirect_is_set(&(settings->stdout))) {

    int fd = shell_redirect_open_file(&(settings->stdout));

    if (fd < 0) {
      free_shell_io_context(context);
      return (ShellIO){.context = NULL, .function = {.fprintf = NULL}};
    }

    context->stdout =
        (ShellIOContextState){.type = ShellIOContextStateTypeRedirect,
                              .data = {.redirect = {.fd = fd}}};
  }

  if (shell_redirect_is_set(&(settings->stderr))) {

    int fd = shell_redirect_open_file(&(settings->stderr));

    if (fd < 0) {
      free_shell_io_context(context);
      return (ShellIO){.context = NULL, .function = {.fprintf = NULL}};
    }

    context->stderr =
        (ShellIOContextState){.type = ShellIOContextStateTypeRedirect,
                              .data = {.redirect = {.fd = fd}}};
  }

  ShellIO io = {.context = context,
                .function = {.fprintf = shell_io_fn_fprintf}};

  return io;
}

static void free_shell_io_in_current_context(ShellIO io) {
  free_shell_io_context(io.context);
}

ExitCode
invoke_shell_builtin_in_current_context(const ShellCommandBuiltin *builtin,
                                        ShellHandle shell,
                                        const ShellRedirectSettings *settings) {

  ShellIO io = get_shell_io_in_current_context(settings);

  if (io.context == NULL) {
    return ExitCodeImplementationError;
  }

  ExitCode command_exit_code =
      builtin->command.invoke(builtin->args, shell, io);

  free_shell_io_in_current_context(io);

  return command_exit_code;
}

static ShellIO
get_shell_io_in_subshell(const ShellRedirectSettings *const settings,
                         const ShellPipeSettings *pipe_settings) {

  ShellIO io = get_shell_io_in_current_context(settings);

  if (io.context == NULL) {
    return io;
  }

  // NOTE: atm no shell builtin needs stdin, so we just drain it at the end

  {
    // handle pipe data

    if (pipe_settings->stdout.is_pipe) {

      ShellIOContext *ctx = io.context;

      if (ctx->stdout.type == ShellIOContextStateTypeNormal) {
        // if we already have a stdout redirection, we don't need a new one, so
        // we just leave the existing one, otherwise we add the pipe fd as
        // redirection
        ctx->stdout = (ShellIOContextState){
            .type = ShellIOContextStateTypeRedirect,
            .data = {
                .redirect = {
                    .fd = pipe_settings->stdout.state.fds[PIPE_WRITE_END]}}};
      }
    }
  }

  // NOTE: we would need to close the unused stdout READ_END and the unused
  // stdin WRITE_END, but the pipeline works in that way, that with pure
  // builtins, that all run in one process, each step closes one part of the
  // pipe, so closing it here would be wrong, and forked executables are going
  // to close it in this process after the fork

  return io;
}

#define DRAIN_BUFFER_SIZE 256

static void drain_stream(int fd) {
  uint8_t drain_buffer[DRAIN_BUFFER_SIZE];

  while (true) {
    ssize_t res = read(fd, drain_buffer, DRAIN_BUFFER_SIZE);

    if (res <= 0) {
      return;
    }
  }
}

static void free_shell_io_in_subshell(ShellIO io,
                                      const ShellPipeSettings *pipe_settings) {

  free_shell_io_in_current_context(io);

  if (pipe_settings->stdin.is_pipe) {
    // if we have a stdin pipe drain it's input and close the stdin pipe
    int stdin_fd = pipe_settings->stdin.state.fds[PIPE_READ_END];

    RETURNS_VOID drain_stream(stdin_fd);

    // TODO: maybe report errors here
    IGNORE_RESULT close(stdin_fd);
  }

  if (pipe_settings->stdout.is_pipe) {
    // if we have a stdout pipe, close it
    int stdout_fd = pipe_settings->stdin.state.fds[PIPE_WRITE_END];

    // TODO: maybe report errors here
    IGNORE_RESULT close(stdout_fd);
  }
}

ExitCode
invoke_shell_builtin_in_subshell(const ShellCommandBuiltin *builtin,
                                 ShellHandle shell,
                                 const ShellRedirectSettings *settings,
                                 const ShellPipeSettings *pipe_settings) {

  ShellIO io = get_shell_io_in_subshell(settings, pipe_settings);

  if (io.context == NULL) {
    return ExitCodeImplementationError;
  }

  ExitCode command_exit_code =
      builtin->command.invoke(builtin->args, shell, io);

  free_shell_io_in_subshell(io, pipe_settings);

  return command_exit_code;
}

NODISCARD bool shell_redirect_is_set(const ShellRedirectLocation *location) {
  return !tstr_is_null(&(location->location));
}

NODISCARD int shell_redirect_open_file(const ShellRedirectLocation *location) {

  // TODO: maybe warn for trunaction, but bash doesn't do that too

  int flags = O_WRONLY | O_CREAT;

  if (location->append) {
    flags |= O_APPEND;
  } else {
    flags |= O_TRUNC;
  }

  int fd = open(tstr_cstr(&(location->location)), flags, 0644);

  return fd;
}

void shell_redirect_close_file(int fd) {
  // TODO: maybe report errors here
  IGNORE_RESULT close(fd);
}
