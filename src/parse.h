
#pragma once

#include "./builtins.h"
#include "./utils.h"

typedef enum : uint8_t {
  ShellParseDataResultTypeError = 0,
  ShellParseDataResultTypeCommand,
  ShellParseDataResultTypeCommandNotFound,
  ShellParseDataResultEmpty,
} ShellParseDataResultType;

typedef struct {
  tstr name;
} ShellCommandNotFound;

typedef enum : uint8_t {
  ShellCommandTypeBuiltin = 0,
  ShellCommandTypeExecutable
} ShellCommandType;

typedef struct {
  tstr path;
  tstr invoke_name;
} ShellCommandExecutable;

typedef struct {
  ShellCommandType type;
  union {
    ShellCommandBuiltin builtin;
    ShellCommandExecutable executable;
  } data;
  ShellArguments args;
  ShellRedirectSettings settings;
} ShellCommand;

typedef struct {
  ShellParseDataResultType type;
  union {
    ShellCommandNotFound command_not_found;
    ShellCommand command;
    tstr error;
  } data;
} ShellParseDataResult;

NODISCARD ShellParseDataResult shell_parse_data(tstr_view input,
                                                const ShellHandle shell);

void free_shell_arguments(ShellArguments *args);

void free_shell_command_not_found(ShellCommandNotFound *found);
