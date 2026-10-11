#include "./history.h"
#include "./builtins.h"

#include <fcntl.h>
#include <unistd.h>

NODISCARD bool read_history_to_file(const tstr* file, read_history_to_file_cb cb,
                                    shell_virtual_context ctx) {

	FILE* f_desc = fopen(tstr_cstr(file), "r");

	if(f_desc) {
		return false;
	}

	char* linebuf = NULL;
	size_t size = 0;

	while(true) {

		ssize_t res = getline(&linebuf, &size, f_desc);

		if(res < 0) {
			free(linebuf);

			if(feof(f_desc) != 0) {
				break;
			}

			IGNORE_RESULT fclose(f_desc);
			return false;
		}

		tstr_view line = (tstr_view){ .data = linebuf, .len = (size_t)res };

		if(tstr_view_ends_with(line, "\n")) {
			line = tstr_sub(line, 0, line.len - 1);
		}

		bool ok = cb(ctx, line);
		if(!ok) {
			free(linebuf);
			IGNORE_RESULT fclose(f_desc);
			return false;
		}
	}

	IGNORE_RESULT fclose(f_desc);
	return true;
}

NODISCARD bool write_history_to_file(const ShellHistory* history, const tstr* file, bool append) {

	int o_flags = O_WRONLY | O_CREAT;

	if(append) {
		o_flags |= O_APPEND;
	} else {
		o_flags |= O_TRUNC;
	}

	int fd = open(tstr_cstr(file), o_flags, OMODE_DEFAULT);

	if(fd < 0) {
		return false;
	}

	for(size_t i = 0; i < TVEC_LENGTH(tstr, history->array); ++i) {

		const tstr* const entry = TVEC_GET_AT(tstr, &(history->array), i);
		ASSERT(entry != NULL);

		ssize_t wres = write(fd, tstr_cstr(entry), tstr_len(entry));

		if(wres < 0) {
			IGNORE_RESULT close(fd);
			return false;
		}

		wres = write(fd, "\n", 1);

		if(wres < 0) {
			IGNORE_RESULT close(fd);
			return false;
		}
	}

	IGNORE_RESULT close(fd);

	return true;
}
