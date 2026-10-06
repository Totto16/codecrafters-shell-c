#include "./shell.h"
#include "./utils.h"

int main(int argc, char *argv[]) {
  // TODO: parse arguments and invoke script or set options regarding to that
  UNUSED(argc);
  UNUSED(argv);

  ExitCode code = shell_execute_repl();

  return (int)code;
}
