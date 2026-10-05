
#pragma once

#include "./builtins.h"

typedef enum : uint8_t {
  ShellParseDataResultTypeError = 0,
  ShellParseDataResultTypeCommand,
  ShellParseDataResultTypeCommandNotFound,
  ShellParseDataResultEmpty, 
} ShellParseDataResultType;

typedef struct {
  tstr_view name;
} ShellCommandNotFound;

typedef enum : uint8_t {
  ShellCommandTypeBuiltin = 0,
} ShellCommandType;

typedef struct {
  BuiltinCommand command;
  shell_dynamic_args args;
} ShellCommandBuiltin;

typedef struct {
  ShellCommandType type;
  union {
    ShellCommandBuiltin builtin;
  } data;
  ShellArguments args;
} ShellCommand;

typedef struct {
  ShellParseDataResultType type;
  union {
    ShellCommandNotFound command_not_found;
    ShellCommand command;
    tstr error;
  } data;
} ShellParseDataResult;

ShellParseDataResult shell_parse_data(tstr_view input);
