#include "./history.h"
#include "./builtins.h"

#include <fcntl.h>
#include <unistd.h>

NODISCARD tstr read_history_to_file(const tstr* file, read_history_to_file_cb cb,
                                    shell_virtual_context ctx) {

	FILE* f_desc = fopen(tstr_cstr(file), "r");

	if(f_desc == NULL) {
		return TSTR_LIT("file open error");
	}

	char* linebuf = NULL;
	size_t size = 0;

	while(true) {

		ssize_t res = getline(&linebuf, &size, f_desc);

		if(res < 0) {
			free(linebuf);

			if(feof(f_desc) != 0) {
				IGNORE_RESULT fclose(f_desc);
				return tstr_null();
			}

			IGNORE_RESULT fclose(f_desc);
			return TSTR_LIT("getline error");
		}

		if(linebuf == NULL) {
			IGNORE_RESULT fclose(f_desc);
			return TSTR_LIT("[INTERNAL] buffer is invalid");
		}

		tstr_view line = (tstr_view){ .data = linebuf, .len = (size_t)res };

		if(tstr_view_ends_with(line, "\n")) {
			line = tstr_sub(line, 0, line.len - 1);
		}

		tstr cb_res = cb(ctx, line);
		if(!tstr_is_null(&cb_res)) {
			free(linebuf);
			IGNORE_RESULT fclose(f_desc);
			return cb_res;
		}
	}
}

NODISCARD static tstr write_history_to_file_impl(const ShellHistory* history, const tstr* file,
                                                 bool append, size_t start_idx) {

	int o_flags = O_WRONLY | O_CREAT;

	if(append) {
		o_flags |= O_APPEND;
	} else {
		o_flags |= O_TRUNC;
	}

	int fd = open(tstr_cstr(file), o_flags, OMODE_DEFAULT);

	if(fd < 0) {
		return TSTR_LIT("file open error");
	}

	for(size_t i = start_idx; i < TVEC_LENGTH(tstr, history->array); ++i) {

		const tstr* const entry = TVEC_GET_AT(tstr, &(history->array), i);
		ASSERT(entry != NULL);

		ssize_t wres = write(fd, tstr_cstr(entry), tstr_len(entry));

		if(wres < 0) {
			IGNORE_RESULT close(fd);
			return TSTR_LIT("write error");
		}

		wres = write(fd, "\n", 1);

		if(wres < 0) {
			IGNORE_RESULT close(fd);
			return TSTR_LIT("write error");
		}
	}

	IGNORE_RESULT close(fd);

	return tstr_null();
}

NODISCARD tstr write_history_to_file(const ShellHistory* history, const tstr* file) {

	return write_history_to_file_impl(history, file, false, 0);
}

NODISCARD tstr append_history_to_file(const ShellHistory* history, const tstr* file,
                                      size_t start_idx) {
	return write_history_to_file_impl(history, file, true, start_idx);
}
