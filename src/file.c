#include "./file.h"
#include "./utils.h"

#include <sys/stat.h>
#include <tvec/tvec.h>
#include <unistd.h>

bool path_is_absolute(tstr_view path) {
	return tstr_view_starts_with(path, "/");
}

NODISCARD tstr_view path_normalize_dir_ending(tstr_view path) {
	if(path.len == 1) {
		// if we just have one "/", don't remove it!
		return path;
	}
	if(tstr_view_ends_with(path, "/")) {
		path = tstr_sub(path, 0, path.len - 1);
	}

	return path;
}

bool path_exists_and_is_dir(tstr_view path) {

	tstr path_str = tstr_from_view(path);

	struct stat stats;
	int res = stat(tstr_cstr(&path_str), &stats);

	tstr_free(&path_str);

	if(res < 0) {
		return false;
	}

	return (S_ISDIR(stats.st_mode)) != 0;
}

static tstr path_join_limbs(const tstr_view* const views, const size_t count) {
	// start with "/", so that every path is an absolute path at the end
	tstr result = tstr_from("/");

	for(size_t i = 0; i < count; ++i) {

		tstr_view view = views[i];

		if(tstr_view_starts_with(view, "/")) {
			view = tstr_sub_until_end(view, 1);
		}
		if(view.len == 0) {
			if(i == 0) {
				// allow it for the first limb
				continue;
			}
			// don't allow this

			tstr_free(&result);

			return tstr_null();
		}

		TStrResult res = tstr_cat_len(&result, view.data, view.len);

		if(res != TStrResultOk) {
			tstr_free(&result);

			return tstr_null();
		}

		if(i < (count - 1) && !tstr_view_ends_with(view, "/")) {
			TStrResult res2 = tstr_cat(&result, "/");

			if(res2 != TStrResultOk) {
				tstr_free(&result);

				return tstr_null();
			}
		}
	}

	return result;
}

static ShellExecutableResult path_dir_get_executable_impl(tstr path) {

	// the full path is in path
	const char* const path_cstr = tstr_cstr(&path);

	struct stat stats;
	int res3 = stat(path_cstr, &stats);

	if(res3 < 0) {
		tstr_free(&path);

		return (ShellExecutableResult){ .is_ok = false,
			                            .data = { .error = {
			                                          .type = ShellExecutableErrorTypeNotFound,
			                                      } } };
	}

	int res4 = access(path_cstr, X_OK);

	if(res4 != 0) {
		tstr_free(&path);

		return (ShellExecutableResult){ .is_ok = false,
			                            .data = { .error = {
			                                          .type = ShellExecutableErrorTypeNoPermission,
			                                      } } };
	}

	return (ShellExecutableResult){ .is_ok = true, .data = { .ok = path } };
}

NODISCARD ShellExecutableResult path_dir_get_executable_from_parts(tstr_view dir, tstr_view name) {
	tstr_view limbs[2] = { dir, name };

	tstr path = path_join_limbs(limbs, ARRAY_SIZE(limbs));

	if(tstr_is_null(&path)) {
		return (ShellExecutableResult){ .is_ok = false,
			                            .data = { .error = {
			                                          .type = ShellExecutableErrorTypeGeneric,
			                                      } } };
	}

	return path_dir_get_executable_impl(path);
}

NODISCARD ShellExecutableResult path_dir_get_executable(tstr_view path) {

	tstr path_str = tstr_from_view(path);

	return path_dir_get_executable_impl(path_str);
}

// TODO(Totto): make compatible with other OSes
#include <linux/limits.h>

tstr path_get_cwd(void) {
	char cwd[PATH_MAX];

	if(getcwd(cwd, sizeof(cwd)) != NULL) {
		return tstr_from(cwd);
	} else {
		return tstr_null();
	}
}

TVEC_DEFINE_AND_IMPLEMENT_VEC_TYPE(tstr_view)

typedef TVEC_TYPENAME(tstr_view) tstr_views;

#define PROCESS_LIMB_ARGUMENT(limb) \
	do { \
\
		if(tstr_view_eq(limb, ".")) { \
			/* ignore*/ \
		} else if(tstr_view_eq(limb, "..")) { \
			/*pop a limb, if possible*/ \
			if(TVEC_LENGTH(tstr_view, limbs) > 0) { \
				TVEC_POP(tstr_view, &(limbs)); \
			} \
		} else { \
			TvecResult res = TVEC_PUSH(tstr_view, &(limbs), limb); \
\
			if(res != TvecResultOk) { /* NOLINT(readability-implicit-bool-conversion)*/ \
				TVEC_FREE(tstr_view, &limbs); \
				return tstr_null(); \
			} \
		} \
	} while(false)

NODISCARD tstr path_normalize(tstr_view path) {

	tstr_views limbs = TVEC_EMPTY(tstr_view);

	size_t start = 0;

	for(size_t i = 0; i < path.len; ++i) {
		const char character = path.data[i];

		if(character == '/') {
			if(start != i) {

				const tstr_view orig = (tstr_view){ .data = path.data + start, .len = i - start };

				PROCESS_LIMB_ARGUMENT(orig);
			}
			start = i + 1;
		}
	}

	if(start < path.len) {
		const tstr_view orig = (tstr_view){ .data = path.data + start, .len = path.len - start };

		PROCESS_LIMB_ARGUMENT(orig);
	}

	tstr final_path =
	    path_join_limbs(TVEC_DATA_CONST(tstr_view, &limbs), TVEC_LENGTH(tstr_view, limbs));

	TVEC_FREE(tstr_view, &limbs);

	if(tstr_is_null(&final_path)) {
		return tstr_null();
	}

	return final_path;
}

static bool path_limb_is_special(tstr_view limb) {
	if(tstr_view_eq(limb, ".")) {
		return true;
	} else if(tstr_view_eq(limb, "..")) {
		return true;
	}

	return false;
}

NODISCARD bool path_is_normalized(tstr_view path) {

	if(path.len == 0) {
		return false;
	}

	if(!tstr_view_starts_with(path, "/")) {
		return false;
	}

	size_t start = 0;

	for(size_t i = 0; i < path.len; ++i) {
		const char character = path.data[i];

		if(character == '/') {
			if(start == 0) {
				// no problem, as the for the start this is valid
			} else if(start == i) {
				// two "/" after another, it is an error
				return false;
			} else {

				const tstr_view orig = (tstr_view){ .data = path.data + start, .len = i - start };

				if(path_limb_is_special(orig)) {
					return false;
				}
			}

			start = i + 1;
		}
	}

	if(start < path.len) {
		const tstr_view orig = (tstr_view){ .data = path.data + start, .len = path.len - start };

		if(path_limb_is_special(orig)) {
			return false;
		}
	}

	return true;
}

static tstr path_resolve_home_dir(tstr_view path) {

	const char* const home_env = getenv("HOME");

	if(home_env == NULL) {
		return tstr_null();
	}

	if(path.len == 0) {
		return tstr_from(home_env);
	}

	tstr_view limbs[2] = { tstr_view_from(home_env), path };

	tstr final_path = path_join_limbs(limbs, ARRAY_SIZE(limbs));

	if(tstr_is_null(&final_path)) {
		return tstr_null();
	}

	return final_path;
}

static tstr path_validate_absolute(tstr path) {
	if(path_is_absolute(tstr_as_view(&path))) {
		return path;
	}

	tstr_free(&path);
	return tstr_null();
}

NODISCARD tstr path_resolve_to_absolute(tstr_view cwd, tstr_view path) {

	if(path.len == 0) {
		// NOTE: should we resolve that to "/" or "$cwd" or should cd handle this?
		return tstr_null();
	}

	const char first_char = path.data[0];
	if(first_char == '/') {
		// absolute path, return it

		return tstr_from_view(path);
	} else if(first_char == '~') {
		// path with ~, resolve that and check that the result is absolute

		path = tstr_sub_until_end(path, 1);
		return path_validate_absolute(path_resolve_home_dir(path));
	} else {
		// we have a relative path

		tstr_view limbs[2] = { cwd, path };

		tstr final_path = path_join_limbs(limbs, ARRAY_SIZE(limbs));

		if(tstr_is_null(&final_path)) {
			return tstr_null();
		}

		return path_validate_absolute(final_path);
	}
}
