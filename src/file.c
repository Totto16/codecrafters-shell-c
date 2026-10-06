#include "./file.h"
#include "./utils.h"

#include <sys/stat.h>
#include <unistd.h>

bool path_is_absolute(tstr_view path) {
  return tstr_view_starts_with(path, "/");
}

NODISCARD tstr_view path_normalize_dir_ending(tstr_view path) {
  if (tstr_view_ends_with(path, "/")) {
    path = tstr_sub(path, 0, path.len - 1);
  }

  return path;
}

bool path_exists_and_is_dir(tstr_view path) {

  tstr path_str = tstr_from_view(path);

  struct stat stats;
  int res = stat(tstr_cstr(&path_str), &stats);

  tstr_free(&path_str);

  if (res < 0) {
    return false;
  }

  return (S_ISDIR(stats.st_mode)) != 0;
}

tstr path_dir_get_executable(tstr_view dir, tstr_view name) {

  tstr path = tstr_from_view(dir);

  TStrResult res = tstr_cat(&path, "/");

  if (res != TStrResultOk) {
    tstr_free(&path);

    return tstr_null();
  }

  TStrResult res2 = tstr_cat_len(&path, name.data, name.len);
  if (res2 != TStrResultOk) {
    tstr_free(&path);

    return tstr_null();
  }

  // the full path is in path
  const char *const path_cstr = tstr_cstr(&path);

  struct stat stats;
  int res3 = stat(path_cstr, &stats);

  if (res3 < 0) {
    tstr_free(&path);

    return tstr_null();
  }

  int res4 = access(path_cstr, X_OK);

  if (res4 != 0) {
    tstr_free(&path);

    return tstr_null();
  }

  return path;
}

// TODO: make compatible with other OSes
#include <linux/limits.h>

tstr path_get_cwd(void) {
  char cwd[PATH_MAX];

  if (getcwd(cwd, sizeof(cwd)) != NULL) {
    return tstr_from(cwd);
  } else {
    return tstr_null();
  }
}
