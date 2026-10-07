#include "./shell.h"
#include "./utils.h"

int main(int argc, char *argv[]) {
  ExitCode code = shell_execute_repl(argc, argv);

  return (int)code;
}
