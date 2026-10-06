#include "./file.h"

#include <sys/stat.h>
#include <unistd.h>

bool path_exists_and_is_dir(tstr_view dir) {

  tstr dir_path = tstr_from_view(dir);

  struct stat stats;
  int res = stat(tstr_cstr(&dir_path), &stats);

  tstr_free(&dir_path);

  if (res < 0) {
    return false;
  }

  return (S_ISDIR(stats.st_mode));
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
