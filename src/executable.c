
#include "./executable.h"
#include "./file.h"
#include "./utils.h"

#include <tvec/tvec.h>

#include <sys/wait.h>
#include <unistd.h>

typedef struct {
  tstr_view path;
} PathDirectory;

TVEC_DEFINE_AND_IMPLEMENT_VEC_TYPE(PathDirectory)

typedef TVEC_TYPENAME(PathDirectory) PathDirectories;

static void free_path_directories(PathDirectories *directories) {
  TVEC_FREE(PathDirectory, directories);
}

typedef struct {
  bool is_ok;
  union {
    PathDirectories directories;
    tstr error;
  } data;
} PathDirectoriesResult;

#ifdef _WIN32
// on windows it is ';'
#define PATH_LIST_SEPARATOR ';'
#else
// on linux, macos unix-like OSes and virtually everywhere else it is ':'
#define PATH_LIST_SEPARATOR ':'
#endif

#define ADD_DIR_ARGUMENT(path_view)                                            \
  do {                                                                         \
                                                                               \
    if (path_is_absolute(path_view)) {                                         \
      tstr_view normalized = path_normalize_dir_ending(path_view);             \
                                                                               \
      if (path_exists_and_is_dir(normalized)) {                                \
        PathDirectory argument = {.path = normalized};                         \
                                                                               \
        TvecResult res = TVEC_PUSH(PathDirectory, &(directories), argument);   \
                                                                               \
        if (res !=                                                             \
            TvecResultOk) { /* NOLINT(readability-implicit-bool-conversion)*/  \
          return (PathDirectoriesResult){                                      \
              .is_ok = false,                                                  \
              .data = {.error =                                                \
                           TSTR_LIT("OOM error in internal PATH parsing")}};   \
        }                                                                      \
      }                                                                        \
    }                                                                          \
  } while (false)

static PathDirectoriesResult get_path_directories(tstr_view path_env) {

  PathDirectories directories = TVEC_EMPTY(PathDirectory);

  // Note: we remove non existent directories and normalize the directories
  // here (ends without "/", no ".." or symlink normalizing, with
  // path_normalize_dir_ending)
  // TODO: use a full normalize for the directories

  // TODO: should we support that?
  //  NOTE: non absolute paths in the PATH env variable are ignore (e.g.
  //  "./my_relative_dir" or "my_relative_dir")

  // TODO: should we support "~" expansion

  size_t start = 0;

  for (size_t i = 0; i < path_env.len; ++i) {
    const char character = path_env.data[i];

    if (character == PATH_LIST_SEPARATOR) {

      if (start != i) {

        const tstr_view orig =
            (tstr_view){.data = path_env.data + start, .len = i - start};

        ADD_DIR_ARGUMENT(orig);
      }
      start = i + 1;
    }
  }

  if (start < path_env.len) {
    const tstr_view orig =
        (tstr_view){.data = path_env.data + start, .len = path_env.len - start};

    ADD_DIR_ARGUMENT(orig);
  }

  return (PathDirectoriesResult){.is_ok = true,
                                 .data = {.directories = directories}};
}

tstr get_executable_path(tstr_view name) {

  const char *const path_env = getenv("PATH");

  if (path_env == NULL) {
    return tstr_null();
  }

  PathDirectoriesResult path_res =
      get_path_directories(tstr_view_from(path_env));

  if (!path_res.is_ok) {
    // NOTe. the error is ignored here
    tstr_free(&path_res.data.error);
    return tstr_null();
  }

  PathDirectories directories = path_res.data.directories;

  for (size_t i = 0; i < TVEC_LENGTH(PathDirectory, directories); ++i) {

    const PathDirectory *const arg =
        TVEC_GET_AT(PathDirectory, &directories, i);

    tstr executable_path = path_dir_get_executable(arg->path, name);

    if (!tstr_is_null(&executable_path)) {
      free_path_directories(&directories);
      return executable_path;
    }
  }

  free_path_directories(&directories);
  return tstr_null();
}

static void free_execv_args(char **const args) {

  for (size_t i = 0;; ++i) {
    char *arg = args[i];

    free(arg);

    if (arg == NULL) {
      break;
    }
  }

  free(args);
}

ExitCode invoke_executable(const tstr *path, const tstr *const invoke_name,
                           const ShellArguments *arguments) {
  // NOTE: this is synchronous, so no need to lock resources (e.g. stdout,
  // stderr)

  pid_t pid = fork();

  if (pid == -1) {
    // TODO: print execute error
    return ExitCodeGenericError;
  }

  if (pid == 0) {
    // Child process: execute the program

    const size_t arguments_length = TVEC_LENGTH(ShellArgument, *arguments);

    char **const args = malloc(sizeof(*args) * (arguments_length + 2));

    {
      tstr invoke_name_dup = tstr_dup(invoke_name);
      args[0] = tstr_take(&invoke_name_dup);
    }

    args[arguments_length + 1] = NULL;

    for (size_t i = 0; i < arguments_length; ++i) {
      const ShellArgument *arg = TVEC_GET_AT(ShellArgument, arguments, i);
      tstr arg_dup = tstr_dup(&(arg->parsed));
      args[i + 1] = tstr_take(&arg_dup);
    }

    int exec_res = execv(tstr_cstr(path), args);

    // Only reached if exec fails
    // TODO: print execute error
    free_execv_args(args);
    UNUSED(exec_res);
    return ExitCodeGenericError;
  }

  // Parent process: wait for child
  int status;
  int res = waitpid(pid, &status, 0);
  if (res < 0) {
    // TODO: print execute error
    return ExitCodeGenericError;
  }

  // TODO: should we return if the process exited with a signal?

  if (WIFEXITED(status)) {
    uint8_t exit_status = WEXITSTATUS(status);
    return exit_status;
  }

  // TODO: print execute error
  return ExitCodeGenericError;
}
