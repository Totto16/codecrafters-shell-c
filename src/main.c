#include "./shell.h"
#include "./utils.h"

int main(int argc, char* argv[]) {
	if(argc < 0) {
		return 69;
	}

	ExitCode code = shell_execute_repl((size_t)argc, argv);

	return (int)code;
}
