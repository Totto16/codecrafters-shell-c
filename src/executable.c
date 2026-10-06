
#include "./executable.h"
#include "./file.h"

#include <tvec/tvec.h>

typedef struct {
  tstr_view path;
} PathDirectory;

TVEC_DEFINE_AND_IMPLEMENT_VEC_TYPE(PathDirectory)

typedef TVEC_TYPENAME(PathDirectory) PathDirectories;

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
    if (tstr_view_starts_with(path_view, "/")) {                               \
                                                                               \
      tstr_view orig = path_view;                                              \
                                                                               \
      if (tstr_view_ends_with(orig, "/")) {                                    \
        orig = tstr_sub(orig, 0, orig.len - 1);                                \
      }                                                                        \
                                                                               \
      if (path_exists_and_is_dir(orig)) {                                      \
        PathDirectory argument = {.path = orig};                               \
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
  // here (ends without "/", no ".." or symlink normalizing)
  // NOTE: non absolute paths in the PATH env variable are ignore (e.g.
  // "./my_relative_dir" or "my_relative_dir")

  size_t start = 0;

  for (size_t i = 0; i < path_env.len; ++i) {
    const char character = path_env.data[i];

    if (character == PATH_LIST_SEPARATOR) {

      if (start != i) {

        tstr_view orig =
            (tstr_view){.data = path_env.data + start, .len = i - start};

        ADD_DIR_ARGUMENT(orig);
      }
      start = i + 1;
    }
  }

  if (start < path_env.len) {
    tstr_view orig =
        (tstr_view){.data = path_env.data + start, .len = path_env.len - start};

    ADD_DIR_ARGUMENT(orig);
  }

  return (PathDirectoriesResult){.is_ok = true,
                                 .data = {.directories = directories}};
}

tstr get_executable_path(tstr_view name) {

  char *path_env = getenv("PATH");

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
      return executable_path;
    }
  }

  return tstr_null();
}
