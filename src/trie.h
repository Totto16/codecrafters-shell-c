#pragma once

#include "./utils.h"

#include <tstr/tstr.h>
#include <tvec/tvec.h>

typedef struct TrieImpl Trie;

NODISCARD Trie* initialize_trie(void);

NODISCARD bool add_to_trie(Trie* trie, tstr value);

typedef struct {
	tstr_view name;
} TrieResultEntry;

TVEC_DEFINE_VEC_TYPE(TrieResultEntry)

typedef TVEC_TYPENAME(TrieResultEntry) TrieResultEntryArray;

typedef struct {
	bool is_ok;
	union {
		TrieResultEntryArray ok;
		tstr error;
	} data;
} TrieSearchResult;

NODISCARD TrieSearchResult search_in_trie(const Trie* trie, tstr_view prefix);

void free_trie(Trie* trie);
