
#include "./parse.h"
#include "./command.h"
#include "./file.h"

TVEC_IMPLEMENT_VEC_TYPE(ShellCommand)

TVEC_IMPLEMENT_VEC_TYPE(ShellCommandNotFound)

typedef struct {
	ShellArgument program;
	ShellArguments args;
	ShellProgramSettings settings;
} ParsedProgram;

TVEC_DEFINE_AND_IMPLEMENT_VEC_TYPE(ParsedProgram)

typedef TVEC_TYPENAME(ParsedProgram) ParsedPrograms;

typedef struct {
	ParsedPrograms programs;
} ParsedPipeline;

typedef struct {
	bool is_ok;
	union {
		ParsedPipeline pipeline;
		tstr error;
	} data;
} ParsedPipelineResult;

#define ADD_SHELL_ARGUMENT(argument) \
	do { \
		TvecResult res = TVEC_PUSH(ShellArgument, &(arguments), (argument)); \
\
		if(res != TvecResultOk) { /* NOLINT(readability-implicit-bool-conversion)*/ \
			return (ParsedPipelineResult){ \
				.is_ok = false, .data = { .error = TSTR_LIT("OOM error in internal parsing") } \
			}; \
		} \
	} while(false)

#define ADD_TO_ACCUMULATOR(argument, add_raw_span) \
	do { \
		TStrResult res = tstr_cat_len(&(acc.str), (argument).data, (argument).len); \
\
		if(res != TStrResultOk) { \
			return (ParsedPipelineResult){ \
				.is_ok = false, .data = { .error = TSTR_LIT("OOM error in internal parsing") } \
			}; \
		} \
		acc.raw.len += add_raw_span; \
	} while(false)

typedef enum : uint8_t {
	ParseStateTypeNormal = 0,
	ParseStateTypeSingleQuote,
	ParseStateTypeDoubleQuote,
	ParseStateTypeEscape,
	ParseStateTypeVariable,
} ParseStateType;

typedef struct {
	tstr str;
	tstr_view raw;
} ParseStateAccumulator;

typedef enum : uint8_t {
	ParseStateDoubleQuoteTypeNormal = 0,
	ParseStateDoubleQuoteTypeEscape,
	ParseStateDoubleQuoteTypeVariable,
} ParseStateDoubleQuoteType;

typedef struct {
	bool uses_brackets;
	bool first;
	size_t start;
} ParseStateDoubleQuoteVariable;

typedef struct {
	ParseStateDoubleQuoteVariable var;
} ParseStateVariable;

typedef struct {
	ParseStateDoubleQuoteType type;
	union {
		ParseStateDoubleQuoteVariable variable;
	} data;
} ParseStateDoubleQuote;

typedef struct {
	ParseStateType type;
	union {
		ParseStateDoubleQuote double_quote;
		ParseStateVariable variable;
	} data;
} ParseState;

#define RETURN_IMPL_ERROR(err) \
	do { \
		return (ParsedPipelineResult){ .is_ok = false, .data = { .error = TSTR_LIT(err) } }; \
\
	} while(false)

typedef struct {
	bool has_value;
	tstr_view value_view;
} ShellResolvedValue;

static ShellResolvedValue shell_get_variable_value(const ShellHandle shell, tstr_view value) {
	tstr key = tstr_from_view(value);
	const ShellVariable* variable = shell.function.get_variable(shell.context, &key);

	tstr_free(&key);

	if(variable == NULL) {
		return (ShellResolvedValue){ .has_value = false, .value_view = TSTR_EMPTY_VIEW };
	}

	return (ShellResolvedValue){ .has_value = true,
		                         .value_view = tstr_as_view(&(variable->value)) };
}

static ParsedPipelineResult shell_parse_input(tstr_view input, const ShellHandle shell) {
	ShellArguments arguments = TVEC_EMPTY(ShellArgument);

	size_t start = 0;
	ParseState state = { .type = ParseStateTypeNormal, .data = {} };
	ParseStateAccumulator acc = { .raw = (tstr_view){ .data = input.data, .len = 0 },
		                          .str = tstr_from("") };

	for(size_t i = 0; i < input.len; ++i) {
		const char character = input.data[i];

		switch(state.type) {
			case ParseStateTypeNormal: {
				if(isspace(character) != 0) {
					if(start == i) {

						// if the accumulator has data, we need to add that data to the
						// arguments, as this is an end delimiter to compound strings ("a'b'c"
						// or similar)
						if(acc.raw.len != 0) {
							ShellArgument argument = { .original = acc.raw, .parsed = acc.str };

							ADD_SHELL_ARGUMENT(argument);

							// we move the tstr out of the accumulator to argument, so no need
							// to free the string
							acc = (ParseStateAccumulator){
								.raw = (tstr_view){ .data = input.data + start, .len = 0 },
								.str = tstr_from("")
							};
						}

						// the next character is a whitespace, so skip it, until we either
						// encounter another character as end delimiter
						acc.raw.data += 1;
						acc.raw.len = 0;

						start = i + 1;
					} else {

						const tstr_view orig =
						    (tstr_view){ .data = input.data + start, .len = i - start };

						const size_t add_raw_span = i - start;

						ADD_TO_ACCUMULATOR(orig, add_raw_span);

						ShellArgument argument = { .original = acc.raw, .parsed = acc.str };

						ADD_SHELL_ARGUMENT(argument);

						start = i + 1;

						// we move the tstr out of the accumulator to argument, so no need to
						// free the string
						acc =
						    (ParseStateAccumulator){ .raw = (tstr_view){ .data = input.data + start,
							                                             .len = 0 },
							                         .str = tstr_from("") };
					}

				} else if(character == '\'') {
					state = (ParseState){ .type = ParseStateTypeSingleQuote, .data = {} };

					if(start == i) {
						// no string before it, just advance start
					} else {
						// if we have some string before ' we need to set the accumulator
						// to it the thing before this is a normal string without quotes,
						// otherwise it would already be consumed
						const tstr_view orig =
						    (tstr_view){ .data = input.data + start, .len = i - start };
						const size_t add_raw_span = (i - start) + 1;

						ADD_TO_ACCUMULATOR(orig, add_raw_span);
					}
					start = i + 1;
				} else if(character == '"') {
					state = (ParseState){ .type = ParseStateTypeDoubleQuote,
						                  .data = { .double_quote = {
						                                .type = ParseStateDoubleQuoteTypeNormal,
						                                .data = {} } } };

					if(start == i) {
						// no string before it, just advance start
					} else {
						// if we have some string before " we need to set the accumulator
						// to it the thing before this is a normal string without quotes,
						// otherwise it would already be consumed
						const tstr_view orig =
						    (tstr_view){ .data = input.data + start, .len = i - start };
						const size_t add_raw_span = (i - start) + 1;

						ADD_TO_ACCUMULATOR(orig, add_raw_span);
					}
					start = i + 1;
				} else if(character == '$') {
					state = (ParseState){ .type = ParseStateTypeVariable,
						                  .data = { .variable = { .var = {
						                                              .uses_brackets = false,
						                                              .first = true,
						                                              .start = i,
						                                          } } } };

					if(start == i) {
						// no string before it, just advance start
					} else {
						// if we have some string before $ we need to set the accumulator
						// to it the thing before this is a normal string without quotes,
						// otherwise it would already be consumed
						const tstr_view orig =
						    (tstr_view){ .data = input.data + start, .len = i - start };
						const size_t add_raw_span = (i - start) + 1;

						ADD_TO_ACCUMULATOR(orig, add_raw_span);
					}
					start = i;
				} else if(character == '\\') {
					state = (ParseState){ .type = ParseStateTypeEscape, .data = {} };

					if(start == i) {
						// no string before it, just advance start
					} else {
						// if we have some string before \ we need to set the accumulator
						// to it the thing before this is a normal string without quotes,
						// otherwise it would already be consumed
						const tstr_view orig =
						    (tstr_view){ .data = input.data + start, .len = i - start };
						const size_t add_raw_span = (i - start) + 1;

						ADD_TO_ACCUMULATOR(orig, add_raw_span);
					}
					start = i + 1;
				}

				break;
			}
			case ParseStateTypeSingleQuote: {
				if(character == '\'') {

					const tstr_view orig =
					    (tstr_view){ .data = input.data + start, .len = i - start };

					const size_t add_raw_span = (i - start) + 2;

					ADD_TO_ACCUMULATOR(orig, add_raw_span);

					state = (ParseState){ .type = ParseStateTypeNormal, .data = {} };

					start = i + 1;
				}
				break;
			}
			case ParseStateTypeDoubleQuote: {
				switch(state.data.double_quote.type) {
					case ParseStateDoubleQuoteTypeNormal: {
						if(character == '"') {
							const tstr_view orig = (tstr_view){ .data = input.data + i, .len = 0 };

							const size_t add_raw_span = 1;

							ADD_TO_ACCUMULATOR(orig, add_raw_span);

							state = (ParseState){ .type = ParseStateTypeNormal, .data = {} };

							start = i + 1;
						} else if(character == '\\') {
							state.data.double_quote =
							    (ParseStateDoubleQuote){ .type = ParseStateDoubleQuoteTypeEscape,
								                         .data = {} };
						} else if(character == '$') {
							state.data.double_quote =
							    (ParseStateDoubleQuote){ .type = ParseStateDoubleQuoteTypeVariable,
								                         .data = { .variable = {
								                                       .uses_brackets = false,
								                                       .first = true,
								                                       .start = i,
								                                   } } };
						} else {
							const tstr_view orig = (tstr_view){ .data = input.data + i, .len = 1 };

							const size_t add_raw_span = 1;

							ADD_TO_ACCUMULATOR(orig, add_raw_span);
						}
						break;
					}
					case ParseStateDoubleQuoteTypeEscape: {

						if(character == '"' || character == '\\' || character == '$' ||
						   character == '`' || character == '\n') {
							const tstr_view orig = (tstr_view){ .data = input.data + i, .len = 1 };

							const size_t add_raw_span = 2;

							ADD_TO_ACCUMULATOR(orig, add_raw_span);

						} else {
							const tstr_view orig =
							    (tstr_view){ .data = input.data + i - 1, .len = 2 };

							const size_t add_raw_span = 2;

							ADD_TO_ACCUMULATOR(orig, add_raw_span);
						}

						state.data.double_quote =
						    (ParseStateDoubleQuote){ .type = ParseStateDoubleQuoteTypeNormal };

						break;
					}
					case ParseStateDoubleQuoteTypeVariable: {

						ParseStateDoubleQuoteVariable* const variable_state =
						    &state.data.double_quote.data.variable;

						if(variable_state->first) {
							variable_state->first = false;

							if(character == '{') {
								variable_state->uses_brackets = true;
							} else {
								if(!shell_is_valid_identifier_start(character)) {
									return (ParsedPipelineResult){
										.is_ok = false,
										.data = { .error =
										              TSTR_LIT("[INTERNAL] invalid data after $: "
										                       "expected valid identifier to be at "
										                       "least one char long") }
									};
								}
							}
						} else {

							bool is_finished = false;

							if(variable_state->uses_brackets) {
								is_finished = character == '}';
							} else {
								// this is more complicated, as we NEED a lookahead
								if(i + 1 < input.len) {
									const char next_character = input.data[i + 1];
									is_finished =
									    !shell_is_valid_identifier_character(next_character);
								} else {
									is_finished = true;
								}
							}

							if(is_finished) {

								const size_t var_start = variable_state->start;

								const tstr_view variable_name = (tstr_view){
									.data = input.data + var_start +
									        (variable_state->uses_brackets ? 2 : 1),
									.len = i - (var_start + (variable_state->uses_brackets ? 2 : 0))
								};

								const size_t add_raw_span = i - var_start;

								ShellResolvedValue resolved_result =
								    shell_get_variable_value(shell, variable_name);

								tstr_view resolved_variable = resolved_result.value_view;

								if(!resolved_result.has_value) {

									// TODO(Totto): when implementing set -u, here we should throw
									// an error, as we are resolving unset variables to an empty
									// string

									resolved_variable = TSTR_TSV("");
								}

								ADD_TO_ACCUMULATOR(resolved_variable, add_raw_span);

								state.data.double_quote =
								    (ParseStateDoubleQuote){ .type =
									                             ParseStateDoubleQuoteTypeNormal };
							}
						}

						break;
					}
					default: {
						return (ParsedPipelineResult){
							.is_ok = false,
							.data = { .error =
							              TSTR_LIT("[INTERNAL] wrong double quote parse state") }
						};
					}
				}
				break;
			}
			case ParseStateTypeEscape: {

				const tstr_view orig = (tstr_view){ .data = input.data + i, .len = 1 };

				const size_t add_raw_span = 1;

				ADD_TO_ACCUMULATOR(orig, add_raw_span);

				state = (ParseState){ .type = ParseStateTypeNormal, .data = {} };

				start = i + 1;

				break;
			}
			case ParseStateTypeVariable: {
				ParseStateDoubleQuoteVariable* const variable_state = &state.data.variable.var;

				if(variable_state->first) {
					variable_state->first = false;

					if(character == '{') {
						variable_state->uses_brackets = true;
					} else {
						if(!shell_is_valid_identifier_start(character)) {
							return (ParsedPipelineResult){
								.is_ok = false,
								.data = { .error = TSTR_LIT("[INTERNAL] invalid data after $: "
								                            "expected valid identifier to be at "
								                            "least one char long") }
							};
						}
					}
				} else {

					bool is_finished = false;

					if(variable_state->uses_brackets) {
						is_finished = character == '}';
					} else {
						// this is more complicated, as we NEED a lookahead
						if(i + 1 < input.len) {
							const char next_character = input.data[i + 1];
							is_finished = !shell_is_valid_identifier_character(next_character);
						} else {
							is_finished = true;
						}
					}

					if(is_finished) {

						const size_t var_start = variable_state->start;

						const tstr_view variable_name = (tstr_view){
							.data =
							    input.data + var_start + (variable_state->uses_brackets ? 2 : 1),
							.len = i - (var_start + (variable_state->uses_brackets ? 2 : 0))
						};

						const size_t add_raw_span = i - var_start;

						ShellResolvedValue resolved_result =
						    shell_get_variable_value(shell, variable_name);

						tstr_view resolved_variable = resolved_result.value_view;

						if(!resolved_result.has_value) {

							// TODO(Totto): when implementing set -u, here we should throw an
							// error, as we are resolving unset variables to an empty string

							// NOTE: in the case of not being in "", we never add to the
							// accumulator, as "
							//  $VAR " should resolve to nothing new, when VAR is not defined

							const size_t acc_start = (size_t)(acc.raw.data - input.data);

							if(acc_start == var_start) {
								// we ignore ans skip this, we advance the accumulator and start, so
								// that it represents the actual start
								//							acc.raw = tstr_sub_until_end(acc.raw,
								// add_raw_span-1);
								acc = (ParseStateAccumulator){
									.raw = (tstr_view){ .data = input.data + i, .len = 0 },
									.str = tstr_from("")
								};
							} else {
								// TODO(Totto): is this correct?
								//  we just add nothing, acc stays the same, start stays the same,
								//  so that the raw string is the one with the variable in it, but
								//  the parsed on has nothing at its space
							}

						} else {
							ADD_TO_ACCUMULATOR(resolved_variable, add_raw_span);
						}

						state = (ParseState){ .type = ParseStateTypeNormal, .data = {} };

						start = i + 1;
					}
				}

				break;
			}
			default: {
				return (ParsedPipelineResult){
					.is_ok = false, .data = { .error = TSTR_LIT("[INTERNAL] wrong parse state") }
				};
			}
		}
	}

	if(state.type != ParseStateTypeNormal) {
		if(state.type == ParseStateTypeSingleQuote) {
			return (ParsedPipelineResult){
				.is_ok = false, .data = { .error = TSTR_LIT("Unterminated single quote") }
			};
		} else if(state.type == ParseStateTypeDoubleQuote) {
			return (ParsedPipelineResult){
				.is_ok = false, .data = { .error = TSTR_LIT("Unterminated double quote") }
			};
		} else if(state.type == ParseStateTypeEscape) {
			return (ParsedPipelineResult){
				.is_ok = false, .data = { .error = TSTR_LIT("Unterminated escape sequence") }
			};
		} else if(state.type == ParseStateTypeVariable) {
			return (ParsedPipelineResult){
				.is_ok = false, .data = { .error = TSTR_LIT("Unterminated variable name") }
			};
		} else {
			return (ParsedPipelineResult){
				.is_ok = false, .data = { .error = TSTR_LIT("[INTERNAL] wrong parse state") }
			};
		}
	}

	if(start < input.len) {
		const tstr_view orig = (tstr_view){ .data = input.data + start, .len = input.len - start };

		const size_t add_raw_span = input.len - start;

		ADD_TO_ACCUMULATOR(orig, add_raw_span);

		ShellArgument argument = { .original = acc.raw, .parsed = acc.str };

		ADD_SHELL_ARGUMENT(argument);

		// we move the tstr out of the accumulator to argument, so no need to
		// free the string
		acc = (ParseStateAccumulator){ .raw = (tstr_view){ .data = input.data + start, .len = 0 },
			                           .str = tstr_from("") };
	}

	// drain the accumulator
	if(acc.raw.len != 0) {
		ShellArgument argument = { .original = acc.raw, .parsed = acc.str };

		ADD_SHELL_ARGUMENT(argument);

		// we move the tstr out of the accumulator to argument, so no need
		// to free the string
		acc = (ParseStateAccumulator){ .raw = (tstr_view){ .data = input.data + start, .len = 0 },
			                           .str = tstr_from("") };
	}

	tstr_free(&acc.str);

	// check for redirect settings, this is easier to do here, as it would get
	// complicated in the parse step

	ParsedPipeline pipeline = { .programs = TVEC_EMPTY(ParsedProgram) };

	if(TVEC_LENGTH(ShellArgument, arguments) == 0) {
		return (ParsedPipelineResult){ .is_ok = true, .data = { .pipeline = pipeline } };
	}

	ParsedProgram
	    current_program = { .program = { .parsed = tstr_null(), .original = TSTR_EMPTY_VIEW },
		                    .args = TVEC_EMPTY(ShellArgument),
		                    .settings = {
		                        .redirect = { .stdout = { .type = ShellRedirectLocationTypeNone },
		                                      .stderr = { .type = ShellRedirectLocationTypeNone } },
		                        .is_background = false,
		                    } };

	for(size_t i = 0; i < TVEC_LENGTH(ShellArgument, arguments); ++i) {

		ShellArgument* const arg = TVEC_GET_AT_MUT(ShellArgument, &(arguments), i);
		ASSERT(arg != NULL);

		// check for supported redirections, use the original string, as quoted
		// strings are not valid
		ShellRedirectLocation* location = NULL;
		bool append = false;
		bool pipe = false;
		bool background = false;

		if(tstr_view_eq(arg->original, ">") || tstr_view_eq(arg->original, "1>")) {
			location = &current_program.settings.redirect.stdout;
			append = false;
		} else if(tstr_view_eq(arg->original, "2>")) {
			location = &current_program.settings.redirect.stderr;
			append = false;
		} else if(tstr_view_eq(arg->original, ">>") || tstr_view_eq(arg->original, "1>>")) {
			location = &current_program.settings.redirect.stdout;
			append = true;
		} else if(tstr_view_eq(arg->original, "2>>")) {
			location = &current_program.settings.redirect.stderr;
			append = true;
		} else if(tstr_view_eq(arg->original, "|")) {
			pipe = true;
		} else if(tstr_view_eq(arg->original, "&")) {
			background = true;
		}

		// TODO
		UNUSED(background);

		if(location != NULL) {

			// assert that we have a valid program before adding a redirection

			if(tstr_is_null(&(current_program.program.parsed))) {
				return (ParsedPipelineResult){
					.is_ok = false,
					.data = { .error = TSTR_LIT("a redirection needs a program to work on") }
				};
			}

			// check that we only have one redirection per program per output stream
			if(shell_redirect_is_set(location)) {
				return (ParsedPipelineResult){
					.is_ok = false,
					.data = { .error = TSTR_LIT(
					              "more than one of the same redirection not supported") }
				};
			}

			// check that we have an argument
			if(i >= TVEC_LENGTH(ShellArgument, arguments) - 1) {
				return (ParsedPipelineResult){
					.is_ok = false, .data = { .error = TSTR_LIT("Unterminated redirection") }
				};
			}

			ShellArgument* const next_arg = TVEC_GET_AT_MUT(ShellArgument, &(arguments), i + 1);
			ASSERT(next_arg != NULL);

			// resolve it to an absolute path, set the location
			{
				tstr_view cwd = shell.function.get_current_working_directory(shell.context);

				if(cwd.len == 0) {
					RETURN_IMPL_ERROR("cwd get error");
				}

				tstr resolved_path =
				    path_resolve_to_absolute(cwd, tstr_as_view(&(next_arg->parsed)));

				if(tstr_is_null(&resolved_path)) {
					RETURN_IMPL_ERROR("path resolve error");
				}

				if(!path_is_absolute(tstr_as_view(&resolved_path))) {
					tstr_free(&resolved_path);
					RETURN_IMPL_ERROR("resolved path is not absolute");
				}

				tstr path = path_normalize(tstr_as_view(&resolved_path));

				tstr_free(&resolved_path);

				if(tstr_is_null(&path)) {
					RETURN_IMPL_ERROR("path normalize error");
				}

				if(!path_is_absolute(tstr_as_view(&path))) {
					tstr_free(&path);
					RETURN_IMPL_ERROR("normalized path is not absolute");
				}

				*location = (ShellRedirectLocation){ .type = ShellRedirectLocationTypeFile,
					                                 .data = { .file = { .location = path,
					                                                     .append = append } } };
			}

			// NOTE: free these two arguments, they are no longer needed, we don't
			// need to remove them from the array, as the array is freed at the end
			// anyways

			free_shell_argument(arg);
			free_shell_argument(next_arg);

		} else if(pipe) {

			// we have a pipe, assert that we have a valid program before it, than add
			// it to the pipeline, and clear out the current program

			if(tstr_is_null(&(current_program.program.parsed))) {
				return (ParsedPipelineResult){ .is_ok = false,
					                           .data = { .error = TSTR_LIT(
					                                         "a pipeline needs a program and can't "
					                                         "follow another pipeline") } };
			}

			// we don't allow background jobs in a pipline
			if(current_program.settings.is_background) {
				return (ParsedPipelineResult){
					.is_ok = false,
					.data = { .error =
					              TSTR_LIT("a pipeline doesn't allow a background job in it") }
				};
			}

			TvecResult res = TVEC_PUSH(ParsedProgram, &(pipeline.programs), current_program);

			if(res != TvecResultOk) { /* NOLINT(readability-implicit-bool-conversion)*/
				return (ParsedPipelineResult){
					.is_ok = false, .data = { .error = TSTR_LIT("OOM error in internal parsing") }
				};
			}

			// NOTE: free this arguments, it is no longer needed, we don't
			// need to remove it from the array, as the array is freed at the end
			// anyways

			free_shell_argument(arg);

			// we moved this out
			current_program =
			    (ParsedProgram){ .program = { .parsed = tstr_null(), .original = TSTR_EMPTY_VIEW },
				                 .args = TVEC_EMPTY(ShellArgument),
				                 .settings = {
				                     .redirect = { .stdout = { .type =
				                                                   ShellRedirectLocationTypeNone,
				                                               .data = {} },
				                                   .stderr = { .type =
				                                                   ShellRedirectLocationTypeNone,
				                                               .data = {} } },
				                     .is_background = false,
				                 } };

		} else if(background) {

			// assert that we have a valid program before setting ti to background

			if(tstr_is_null(&(current_program.program.parsed))) {
				return (ParsedPipelineResult){
					.is_ok = false,
					.data = { .error = TSTR_LIT("a background jobs needs a program to work on") }
				};
			}

			// check that we  haven't set this already
			if(current_program.settings.is_background) {
				return (ParsedPipelineResult){
					.is_ok = false,
					.data = { .error = TSTR_LIT(
					              "can only set a program to be a background job once") }
				};
			}

			current_program.settings.is_background = true;
		} else {
			// append the argument to the end

			if(tstr_is_null(&current_program.program.parsed)) {
				current_program.program = *arg;
			} else {
				TvecResult res = TVEC_PUSH(ShellArgument, &(current_program.args), *arg);

				if(res != TvecResultOk) { /* NOLINT(readability-implicit-bool-conversion)*/
					return (ParsedPipelineResult){
						.is_ok = false,
						.data = { .error = TSTR_LIT("OOM error in internal parsing") }
					};
				}
			}
		}
	}

	// we moved all the arguments or already freed them, so we need to only free
	// the vector itself
	TVEC_FREE(ShellArgument, &arguments);

	if(tstr_is_null(&(current_program.program.parsed))) {

		// TODO(Totto): free state before returning an error

		return (ParsedPipelineResult){
			.is_ok = false,
			.data = { .error = TSTR_LIT("missing program at the end of a pipeline") }
		};
	} else {
		// we add the last program to the pipeline

		TvecResult res = TVEC_PUSH(ParsedProgram, &(pipeline.programs), current_program);

		if(res != TvecResultOk) { /* NOLINT(readability-implicit-bool-conversion)*/
			return (ParsedPipelineResult){
				.is_ok = false, .data = { .error = TSTR_LIT("OOM error in internal parsing") }
			};
		}

		// here we allow the program to be a background job, as it is technically not a pipline

		// we moved this out
		current_program =
		    (ParsedProgram){ .program = { .parsed = tstr_null(), .original = TSTR_EMPTY_VIEW },
			                 .args = TVEC_EMPTY(ShellArgument),
			                 .settings = {
			                     .redirect = { .stdout = { .type = ShellRedirectLocationTypeNone,
			                                               .data = {} },
			                                   .stderr = { .type = ShellRedirectLocationTypeNone,
			                                               .data = {} } },
			                     .is_background = false,
			                 } };
	}

	return (ParsedPipelineResult){ .is_ok = true, .data = { .pipeline = pipeline } };
}

void free_shell_argument(ShellArgument* arg) {
	tstr_free(&(arg->parsed));
}

static void free_shell_redirect_settings(ShellRedirectSettings* settings) {
	free_shell_redirect_location(&(settings->stdout));
	free_shell_redirect_location(&(settings->stderr));
}

static void free_shell_program_settings(ShellProgramSettings* settings) {
	free_shell_redirect_settings(&(settings->redirect));
}

void free_shell_arguments(ShellArguments* args) {

	for(size_t i = 0; i < TVEC_LENGTH(ShellArgument, *args); ++i) {

		ShellArgument* arg = TVEC_GET_AT_MUT(ShellArgument, args, i);
		ASSERT(arg != NULL);

		free_shell_argument(arg);
	}

	TVEC_FREE(ShellArgument, args);
}

static void free_parsed_program(ParsedProgram* program) {
	free_shell_arguments(&(program->args));
	free_shell_argument(&(program->program));
	free_shell_program_settings(&(program->settings));
}

static void free_shell_command_executable(ShellCommandExecutable* executable) {
	tstr_free(&(executable->invoke_name));
	tstr_free(&(executable->path));
}

static void free_shell_command(ShellCommand* command) {
	free_shell_argument(&(command->arg0));
	free_shell_arguments(&(command->args));
	free_shell_program_settings(&(command->settings));

	switch(command->type) {
		case ShellCommandTypeBuiltin: {
			//
			break;
		}
		case ShellCommandTypeExecutable: {
			free_shell_command_executable(&(command->data.executable));

			break;
		}
		default: {
			//
			break;
		}
	}
}

void free_shell_completion_data_ok(ShellCompletionDataOk* data) {
	free_shell_argument(&(data->arg0));
	free_shell_arguments(&(data->args));
}

void free_shell_pipeline(ShellParseDataPipeline* pipeline) {
	for(size_t i = 0; i < TVEC_LENGTH(ShellCommand, pipeline->commands); ++i) {

		ShellCommand* arg = TVEC_GET_AT_MUT(ShellCommand, &(pipeline->commands), i);
		ASSERT(arg != NULL);

		free_shell_command(arg);
	}

	TVEC_FREE(ShellCommand, &(pipeline->commands));
}

static void free_shell_command_not_found(ShellCommandNotFound* not_found) {
	tstr_free(&(not_found->name));
}

void free_shell_commands_not_found(ShellCommandsNotFound* not_found) {
	for(size_t i = 0; i < TVEC_LENGTH(ShellCommandNotFound, not_found->array); ++i) {

		ShellCommandNotFound* arg = TVEC_GET_AT_MUT(ShellCommandNotFound, &(not_found->array), i);
		ASSERT(arg != NULL);

		free_shell_command_not_found(arg);
	}

	TVEC_FREE(ShellCommandNotFound, &(not_found->array));
}

typedef enum : uint8_t {
	ShellProcessProgramResultTypeError = 0,
	ShellProcessProgramResultTypeCommandNotFound,
	ShellProcessProgramResultTypeOk,
} ShellProcessProgramResultType;

typedef struct {
	ShellProcessProgramResultType type;
	union {
		ShellCommand ok;
		ShellCommandNotFound not_found;
		tstr error;
	} data;
} ShellProcessProgramResult;

static ShellProcessProgramResult shell_process_program(ParsedProgram* program,
                                                       const ShellHandle shell) {
	const tstr* const prog_name = &(program->program.parsed);
	const ShellArguments* const prog_args = &(program->args);

	ShellResolvedCommand resolved_command = shell_resolve_command(tstr_as_view(prog_name), shell);

	switch(resolved_command.type) {
		case CommandTypeBuiltin: {
			BuiltinCommand builtin_command = resolved_command.data.builtin;

			ShellParseArgumentsResult args_res = builtin_command.validator.parse(prog_args);

			if(!args_res.is_ok) {
				free_parsed_program(program);

				// move args_res.data.error ownership to the ShellParseDataResult struct
				return (ShellProcessProgramResult){ .type = ShellProcessProgramResultTypeError,
					                                .data = { .error = args_res.data.error } };
			}

			if(args_res.data.args == NULL) {
				free_parsed_program(program);

				return (ShellProcessProgramResult){ .type = ShellProcessProgramResultTypeError,
					                                .data = { .error = TSTR_LIT(
					                                              "[INTERNAL] Parse error") } };
			}

			ShellCommandBuiltin builtin_args = { .command = builtin_command,
				                                 .args = args_res.data.args };

			ShellCommand command = { .type = ShellCommandTypeBuiltin,
				                     .data = { .builtin = builtin_args },
				                     .arg0 = program->program,
				                     .args = program->args,
				                     .settings = program->settings };

			return (ShellProcessProgramResult){ .type = ShellProcessProgramResultTypeOk,
				                                .data = { .ok = command } };
		}
		case CommandTypeExecutable: {

			// we transfer ownership of command_type.data.executable.path to executable,
			// so no need to free that
			const ShellCommandExecutable executable = {
				.path = resolved_command.data.executable.path,
				.invoke_name = tstr_dup(&(program->program.parsed)),

			};

			ShellCommand command = { .type = ShellCommandTypeExecutable,
				                     .data = { .executable = executable },
				                     .arg0 = program->program,
				                     .args = program->args,
				                     .settings = program->settings };

			return (ShellProcessProgramResult){ .type = ShellProcessProgramResultTypeOk,
				                                .data = { .ok = command } };
		}
		case CommandTypeNotFound: {
			ShellCommandNotFoundData not_found_data = resolved_command.data.not_found;

			free_shell_arguments(&program->args);
			free_shell_program_settings(&program->settings);

			ShellCommandNotFound not_found = { .name = program->program.parsed,
				                               .reason = not_found_data.reason };

			return (ShellProcessProgramResult){ .type =
				                                    ShellProcessProgramResultTypeCommandNotFound,
				                                .data = { .not_found = not_found } };
		}
		default: {
			return (ShellProcessProgramResult){
				.type = ShellProcessProgramResultTypeError,
				.data = { .error = TSTR_LIT("[INTERNAL] invalid command type") }
			};
		}
	}
}

ShellParseDataResult shell_parse_data(tstr_view input, const ShellHandle shell) {

	ParsedPipelineResult program_res = shell_parse_input(input, shell);

	if(!program_res.is_ok) {
		// move program_res.data.error ownership to the ShellParseDataResult struct
		return (ShellParseDataResult){ .type = ShellParseDataResultTypeError,
			                           .data = { .error = program_res.data.error } };
	}

	// TODO(Totto): don't add empty entries
	// for that we need the result of the parser first
	bool ok = shell.function.add_to_history(shell.context, input);
	if(!ok) {
		// move program_res.data.error ownership to the ShellParseDataResult struct
		return (ShellParseDataResult){ .type = ShellParseDataResultTypeError,
			                           .data = { .error = TSTR_LIT("history add error") } };
	}

	ParsedPipeline parsed_pipeline = program_res.data.pipeline;

	if(TVEC_LENGTH(ParsedProgram, parsed_pipeline.programs) == 0) {
		return (ShellParseDataResult){ .type = ShellParseDataResultTypeEmpty, .data = {} };
	}

	ShellCommandsNotFound not_found = { .array = TVEC_EMPTY(ShellCommandNotFound) };
	ShellParseDataPipeline pipeline = { .commands = TVEC_EMPTY(ShellCommand) };

	for(size_t i = 0; i < TVEC_LENGTH(ParsedProgram, parsed_pipeline.programs); ++i) {

		ParsedProgram* const arg = TVEC_GET_AT_MUT(ParsedProgram, &(parsed_pipeline.programs), i);
		ASSERT(arg != NULL);

		ShellProcessProgramResult process_res = shell_process_program(arg, shell);

		switch(process_res.type) {
			case ShellProcessProgramResultTypeOk: {
				TvecResult res = TVEC_PUSH(ShellCommand, &pipeline.commands, process_res.data.ok);

				if(res != TvecResultOk) { /* NOLINT(readability-implicit-bool-conversion)*/
					return (ShellParseDataResult){
						.type = ShellParseDataResultTypeError,
						.data = { .error = TSTR_LIT("OOM error in internal parsing") }
					};
				}
				break;
			}
			case ShellProcessProgramResultTypeCommandNotFound: {
				TvecResult res =
				    TVEC_PUSH(ShellCommandNotFound, &not_found.array, process_res.data.not_found);

				if(res != TvecResultOk) { /* NOLINT(readability-implicit-bool-conversion)*/
					return (ShellParseDataResult){
						.type = ShellParseDataResultTypeError,
						.data = { .error = TSTR_LIT("OOM error in internal parsing") }
					};
				}
				break;
			}
			case ShellProcessProgramResultTypeError: {
				// TODO(Totto): collect errors much like command not found, not just report one
				return (ShellParseDataResult){ .type = ShellParseDataResultTypeError,
					                           .data = { .error = process_res.data.error } };
			}
			default: {
				return (ShellParseDataResult){ .type = ShellParseDataResultTypeError,
					                           .data = { .error = TSTR_LIT(
					                                         "[INTERNAL] wrong error type") } };
			}
		}
	}

	// we moved all the arguments or already freed them, so we need to only free
	// the vector itself
	TVEC_FREE(ParsedProgram, &parsed_pipeline.programs);

	// NOTE: we report an error with all commands that are not found, when at
	// least one command in the pipeline can't be found, bash stills runs the
	// parts of the found commands, we do NOT
	if(TVEC_LENGTH(ShellCommandNotFound, not_found.array) != 0) {
		// we found some errors, return those

		free_shell_pipeline(&pipeline);

		return (ShellParseDataResult){ .type = ShellParseDataResultTypeCommandsNotFound,
			                           .data = { .not_found = not_found } };
	}

	free_shell_commands_not_found(&not_found);

	return (ShellParseDataResult){ .type = ShellParseDataResultTypePipeline,
		                           .data = { .pipeline = pipeline } };
}

ShellCompletionDataResult shell_parse_data_for_completion(tstr_view input,
                                                          const ShellHandle shell) {

	ParsedPipelineResult program_res = shell_parse_input(input, shell);

	if(!program_res.is_ok) {
		// move program_res.data.error ownership to the ShellParseDataResult struct
		return (ShellCompletionDataResult){ .is_ok = false,
			                                .data = { .error = program_res.data.error } };
	}

	ParsedPipeline parsed_pipeline = program_res.data.pipeline;

	const size_t pipeline_length = TVEC_LENGTH(ParsedProgram, parsed_pipeline.programs);

	if(pipeline_length == 0) {
		// TODO(Totto): free the pipline

		return (ShellCompletionDataResult){ .is_ok = false,
			                                .data = { .error = TSTR_LIT("Empty command line") } };
	}

	ParsedProgram* const program =
	    TVEC_GET_AT_MUT(ParsedProgram, &(parsed_pipeline.programs), (pipeline_length - 1));
	ASSERT(program != NULL);

	ShellCompletionDataOk ok_data = { .arg0 = program->program, .args = program->args };

	// reset the program args, as we moved out of them
	program->program = (ShellArgument){ .parsed = tstr_null(), .original = TSTR_EMPTY_VIEW };

	program->args = TVEC_EMPTY(ShellArgument);

	// free the pipline
	{
		for(size_t i = 0; i < TVEC_LENGTH(ParsedProgram, parsed_pipeline.programs); ++i) {

			ParsedProgram* const arg =
			    TVEC_GET_AT_MUT(ParsedProgram, &(parsed_pipeline.programs), i);
			ASSERT(arg != NULL);

			free_parsed_program(arg);
		}

		TVEC_FREE(ParsedProgram, &parsed_pipeline.programs);
	}

	return (ShellCompletionDataResult){ .is_ok = true,
		                                .data = {
		                                    .ok = ok_data,
		                                } };
}

bool shell_is_valid_identifier_start(const char start) {
	if(start == '_') {
		return true;
	}

	return isalpha(start) != 0;
}

bool shell_is_valid_identifier_character(const char character) {
	if(character == '_') {
		return true;
	}

	return isalnum(character) != 0;
}
