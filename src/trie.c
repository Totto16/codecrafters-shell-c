
#include "./trie.h"

#include <tmap/tmap.h>

typedef struct {
	tstr name;
} TrieEntry;

TVEC_DEFINE_AND_IMPLEMENT_VEC_TYPE(TrieEntry)

typedef TVEC_TYPENAME(TrieEntry) TrieEntryArray;

typedef enum : uint8_t {
	TrieNodeEntryTypeValue = 0,
	TrieNodeEntryTypeBranch,
	TrieNodeEntryTypeBranchAndValue
} TrieNodeEntryType;

typedef struct {
	size_t idx;
} TrieNodeEntryValue;

TVEC_DEFINE_AND_IMPLEMENT_VEC_TYPE(TrieNodeEntryValue)

typedef TVEC_TYPENAME(TrieNodeEntryValue) TrieNodeEntryValueArray;

typedef struct TrieNodeEntryHashMapOpaqueImpl TrieNodeEntryHashMapOpaque;

typedef struct {
	TrieNodeEntryHashMapOpaque* opaque;
} TrieNodeEntryBranchTrie;

typedef struct {
	TrieNodeEntryValueArray entries;
} TrieNodeEntryBranchLinear;

typedef struct {
	bool is_trie_branch;
	union {
		TrieNodeEntryBranchTrie trie;
		TrieNodeEntryBranchLinear linear;
	} data;
} TrieNodeEntryBranch;

typedef struct {
	TrieNodeEntryBranch branch;
	TrieNodeEntryValue value;
} TrieNodeEntryBranchAndValue;

typedef struct {
	TrieNodeEntryType type;
	union {
		TrieNodeEntryValue value;
		TrieNodeEntryBranch branch;
		// this is needed if e.g. ex and extra are inside, it could be solved in another way, but i
		// prefer it like that
		TrieNodeEntryBranchAndValue branch_and_value;
	} data;
} TrieNodeEntry;

typedef char TrieIdentifierType;

TMAP_DEFINE_MAP_TYPE(TrieIdentifierType, TrieIdentifier, TrieNodeEntry, TrieNodeEntryHashMap)

typedef TMAP_TYPENAME_MAP(TrieNodeEntryHashMap) TrieNodeEntryHashMap;

struct TrieNodeEntryHashMapOpaqueImpl {
	TrieNodeEntryHashMap map;
};

TMAP_HASH_FUNC_SIG(TrieIdentifierType, TrieIdentifier) {
	return TMAP_HASH_SCALAR(key);
}

TMAP_EQ_FUNC_SIG(TrieIdentifierType, TrieIdentifier) {
	return key1 == key2;
}

// NOTE: normal tries have a fixed array instead of a hashmap, but i find it better to use a hashmap
// (no actual reasoning was made)
typedef struct {
	TrieNodeEntryHashMap map;
} TrieRootNode;

struct TrieImpl {
	TrieEntryArray array;
	TrieRootNode root;
};

NODISCARD Trie* initialize_trie(void) {
	Trie* trie = malloc(sizeof(Trie));

	*trie = (Trie){ .array = TVEC_EMPTY(TrieEntry),
		            .root = { .map = TMAP_EMPTY(TrieNodeEntryHashMap) } };

	return trie;
}

// chosen arbitrary
#define TRIE_MAX_LINEAR_SIZE 64

NODISCARD static bool add_value_to_trie_hash_map_impl(TrieNodeEntryHashMap* hash_map,
                                                      tstr_view value, TrieNodeEntryValue entry,
                                                      size_t current_depth) {

	ASSERT(value.len > current_depth);

	const TrieIdentifierType key = value.data[current_depth];

	TrieNodeEntry* map_entry = TMAP_INSERT_SLOT(TrieNodeEntryHashMap, hash_map, key, false);
	ASSERT(map_entry != NULL);

	if(map_entry != TMAP_WOULD_OVERWRITE) {
		// we have no entry with this prefix yet, so add one

		// initialize basic entry, this never needs complicate "re-balances" or similar

		// just a basic linear branch for the start
		TrieNodeEntryBranch branch = {
			.is_trie_branch = false,
			.data = { .linear = { .entries = TVEC_EMPTY(TrieNodeEntryValue) } }
		};

		if(value.len == (current_depth + 1)) {
			// always insert a branch at the first level, never just a value, even if this command
			// might be a value, if it is not the first level, insert a value, as we likely don't
			// need a branch

			if(current_depth == 0) {

				*map_entry = (TrieNodeEntry){ .type = TrieNodeEntryTypeBranchAndValue,
					                          .data = { .branch_and_value = {
					                                        .branch = branch,
					                                        .value = entry,
					                                    } } };
			} else {
				*map_entry =
				    (TrieNodeEntry){ .type = TrieNodeEntryTypeValue, .data = { .value = entry } };
			}
		} else {

			// NOTE: only add the value, if it's not already in this node
			TvecResult res2 = TVEC_PUSH(TrieNodeEntryValue, &(branch.data.linear.entries), entry);

			if(res2 != TvecResultOk) { // NOLINT(readability-implicit-bool-conversion)
				return false;
			}

			*map_entry = (TrieNodeEntry){ .type = TrieNodeEntryTypeBranch,
				                          .data = {
				                              .branch = branch,
				                          } };
		}

		return true;
	}

	// we already have an entry, so we need to get that entry and modify it

	map_entry = TMAP_INSERT_SLOT(TrieNodeEntryHashMap, hash_map, key, true);
	ASSERT(map_entry != NULL);
	ASSERT(map_entry != TMAP_WOULD_OVERWRITE);

	// if the value is at the current depth, we need to set it
	if(value.len == (current_depth + 1)) {

		if(map_entry->type == TrieNodeEntryTypeValue) {
			// we already have a value, it should be the same
			// TODO: we have it duplicated in the array, which shoudln't happen, maybe just call
			// search before inserting?

			// TODO: check if it's the same value?
			return true;
		} else if(map_entry->type == TrieNodeEntryTypeBranchAndValue) {
			// we already have a value, it should be the same
			// TODO: we have it duplicated in the array, which shoudln't happen, maybe just call
			// search before inserting?

			// TODO: check if it's the same value?
			return true;
		} else if(map_entry->type == TrieNodeEntryTypeBranch) {

			TrieNodeEntryBranch old_branch = map_entry->data.branch;

			*map_entry = (TrieNodeEntry){ .type = TrieNodeEntryTypeBranchAndValue,
				                          .data = { .branch_and_value = {
				                                        .branch = old_branch,
				                                        .value = entry,
				                                    } } };

			return true;

		} else {
			// impl error;
			return false;
		}

		UNREACHABLE();
	}

	// the value needs to be inserted into the branch and is not at the current_depth

	if(map_entry->type == TrieNodeEntryTypeValue) {
		// if we have a value here, we make it into a branch with value

		TrieNodeEntryValue old_entry = map_entry->data.value;

		TrieNodeEntryBranch branch = {
			.is_trie_branch = false,
			.data = { .linear = { .entries = TVEC_EMPTY(TrieNodeEntryValue) } }
		};

		*map_entry = (TrieNodeEntry){ .type = TrieNodeEntryTypeBranchAndValue,
			                          .data = { .branch_and_value = {
			                                        .branch = branch,
			                                        .value = old_entry,
			                                    } } };
	}

	// get the branch as ptr
	TrieNodeEntryBranch* current_branch = NULL;
	if(map_entry->type == TrieNodeEntryTypeBranchAndValue) {
		current_branch = &(map_entry->data.branch_and_value.branch);
	} else if(map_entry->type == TrieNodeEntryTypeBranch) {
		current_branch = &(map_entry->data.branch);
	} else {
		// Impl error
		return false;
	}

	ASSERT(current_branch != NULL);

	// insert the new value
	if(current_branch->is_trie_branch) {

		// call this function recursively

		return add_value_to_trie_hash_map_impl(&(current_branch->data.trie.opaque->map), value,
		                                       entry, current_depth + 1);
	}

	TrieNodeEntryBranchLinear* linear_entry = &(current_branch->data.linear);

	// we just have a linear insert
	TvecResult res2 = TVEC_PUSH(TrieNodeEntryValue, &(linear_entry->entries), entry);

	if(res2 != TvecResultOk) { // NOLINT(readability-implicit-bool-conversion)
		return false;
	}

	// check if we need to re-balance the branch

	const size_t array_size = TVEC_LENGTH(TrieNodeEntryValue, linear_entry->entries);

	if(array_size >= TRIE_MAX_LINEAR_SIZE) {

		// TODO: re-balance this

		return false;
	}

	return true;
}

NODISCARD bool add_to_trie(Trie* trie, tstr value) {

	// don't support the empty command

	if(tstr_len(&value) == 0) {
		return false;
	}

	const size_t index = TVEC_LENGTH(TrieEntry, trie->array);

	{
		TrieEntry entry = { .name = value };

		TvecResult res = TVEC_PUSH(TrieEntry, &(trie->array), entry);

		if(res != TvecResultOk) { // NOLINT(readability-implicit-bool-conversion)
			return false;
		}
	}

	TrieNodeEntryValue entry = { .idx = index };

	return add_value_to_trie_hash_map_impl(&(trie->root.map), tstr_as_view(&value), entry, 0);
}

NODISCARD static tstr
trie_add_matches_to_array_all_linear_impl(const TrieEntryArray* entries,
                                          TrieResultEntryArray* array,
                                          const TrieNodeEntryBranchLinear* linear) {
	for(size_t i = 0; i < TVEC_LENGTH(TrieNodeEntryValue, linear->entries); ++i) {

		const TrieNodeEntryValue* const arg =
		    TVEC_GET_AT(TrieNodeEntryValue, &(linear->entries), i);
		ASSERT(arg != NULL);

		const TrieEntry* const entry_value = TVEC_GET_AT(TrieEntry, entries, arg->idx);
		if(entry_value == NULL) {
			return TSTR_LIT("invalid entry index");
		}

		TrieResultEntry entry = { .name = tstr_as_view(&(entry_value->name)) };

		TvecResult res = TVEC_PUSH(TrieResultEntry, array, entry);

		if(res != TvecResultOk) { // NOLINT(readability-implicit-bool-conversion)
			return TSTR_LIT("OOM in argument allocation");
		}
	}

	return tstr_null();
}

static tstr trie_add_matches_to_array_all_branch_impl(const TrieEntryArray* entries,
                                                      TrieResultEntryArray* array,
                                                      const TrieNodeEntryBranch* branch);

NODISCARD static tstr trie_add_matches_to_array_all_trie_impl(const TrieEntryArray* entries,
                                                              TrieResultEntryArray* array,
                                                              const TrieNodeEntryBranchTrie* trie) {

	TMAP_TYPENAME_ITER(TrieNodeEntryHashMap)
	iter = TMAP_ITER_INIT(TrieNodeEntryHashMap, &(trie->opaque->map));

	TMAP_TYPENAME_ENTRY(TrieNodeEntryHashMap) value;

	while(TMAP_ITER_NEXT(TrieNodeEntryHashMap, &iter, &value)) {
		const TrieNodeEntry entry = value.value;

		const TrieNodeEntryBranch* current_branch = NULL;
		if(entry.type == TrieNodeEntryTypeValue) {
			// add the value and return

			const TrieEntry* const entry_value =
			    TVEC_GET_AT(TrieEntry, entries, entry.data.value.idx);
			if(entry_value == NULL) {
				return TSTR_LIT("invalid entry index");
			}

			TrieResultEntry result_entry = { .name = tstr_as_view(&(entry_value->name)) };

			TvecResult res = TVEC_PUSH(TrieResultEntry, array, result_entry);

			if(res != TvecResultOk) { // NOLINT(readability-implicit-bool-conversion)
				return TSTR_LIT("OOM in argument allocation");
			}

			continue;
		} else if(entry.type == TrieNodeEntryTypeBranchAndValue) {
			// add the value an process the branch

			const TrieEntry* const entry_value =
			    TVEC_GET_AT(TrieEntry, entries, entry.data.branch_and_value.value.idx);
			if(entry_value == NULL) {
				return TSTR_LIT("invalid entry index");
			}

			TrieResultEntry result_entry = { .name = tstr_as_view(&(entry_value->name)) };

			TvecResult res = TVEC_PUSH(TrieResultEntry, array, result_entry);

			if(res != TvecResultOk) { // NOLINT(readability-implicit-bool-conversion)
				return TSTR_LIT("OOM in argument allocation");
			}

			current_branch = &(entry.data.branch_and_value.branch);
		} else if(entry.type == TrieNodeEntryTypeBranch) {
			current_branch = &(entry.data.branch);
		} else {
			return TSTR_LIT("Invalid node entry type");
		}

		tstr res = trie_add_matches_to_array_all_branch_impl(entries, array, current_branch);
		if(!tstr_is_null(&res)) {
			return res;
		}
	}

	return tstr_null();
}

NODISCARD static tstr trie_add_matches_to_array_all_branch_impl(const TrieEntryArray* entries,
                                                                TrieResultEntryArray* array,
                                                                const TrieNodeEntryBranch* branch) {
	if(branch->is_trie_branch) {
		return trie_add_matches_to_array_all_trie_impl(entries, array, &(branch->data.trie));
	} else {
		return trie_add_matches_to_array_all_linear_impl(entries, array, &(branch->data.linear));
	}
}

// TODO: do that v
//  NOTE: this is a tstr helper function, that I should eventually add to tstr
NODISCARD static bool tstr_view_starts_with_view(tstr_view view, tstr_view prefix) {
	if(prefix.len > view.len) {
		return false;
	}
	return memcmp(view.data, prefix.data, prefix.len) == 0;
}

NODISCARD static tstr trie_add_matches_to_array_with_prefix_linear_impl(
    const TrieEntryArray* entries, TrieResultEntryArray* array,
    const TrieNodeEntryBranchLinear* linear, tstr_view prefix) {
	for(size_t i = 0; i < TVEC_LENGTH(TrieNodeEntryValue, linear->entries); ++i) {

		const TrieNodeEntryValue* const arg =
		    TVEC_GET_AT(TrieNodeEntryValue, &(linear->entries), i);
		ASSERT(arg != NULL);

		const TrieEntry* const entry_value = TVEC_GET_AT(TrieEntry, entries, arg->idx);
		if(entry_value == NULL) {
			return TSTR_LIT("invalid entry index");
		}

		TrieResultEntry entry = { .name = tstr_as_view(&(entry_value->name)) };

		if(!tstr_view_starts_with_view(entry.name, prefix)) {
			continue;
		}

		TvecResult res = TVEC_PUSH(TrieResultEntry, array, entry);

		if(res != TvecResultOk) { // NOLINT(readability-implicit-bool-conversion)
			return TSTR_LIT("OOM in argument allocation");
		}
	}

	return tstr_null();
}

NODISCARD static tstr trie_add_matches_to_array_with_prefix_impl(
    const TrieEntryArray* entries, TrieResultEntryArray* array,
    const TrieNodeEntryHashMap* const hash_map, tstr_view prefix, size_t current_depth) {

	ASSERT(prefix.len > current_depth);

	const TrieIdentifierType key = prefix.data[current_depth];

	const TrieNodeEntry* const result = TMAP_GET(TrieNodeEntryHashMap, hash_map, key);

	if(result == NULL) {
		// found nothing, just return
		return tstr_null();
	}

	const TrieNodeEntryBranch* current_branch = NULL;
	if(result->type == TrieNodeEntryTypeValue) {
		// add the value and return

		const TrieEntry* const entry_value =
		    TVEC_GET_AT(TrieEntry, entries, result->data.value.idx);
		if(entry_value == NULL) {
			return TSTR_LIT("invalid entry index");
		}

		TrieResultEntry entry = { .name = tstr_as_view(&(entry_value->name)) };

		TvecResult res = TVEC_PUSH(TrieResultEntry, array, entry);

		if(res != TvecResultOk) { // NOLINT(readability-implicit-bool-conversion)
			return TSTR_LIT("OOM in argument allocation");
		}

		return tstr_null();
	} else if(result->type == TrieNodeEntryTypeBranchAndValue) {
		// add the value an process the branch

		const TrieEntry* const entry_value =
		    TVEC_GET_AT(TrieEntry, entries, result->data.branch_and_value.value.idx);
		if(entry_value == NULL) {
			return TSTR_LIT("invalid entry index");
		}

		TrieResultEntry entry = { .name = tstr_as_view(&(entry_value->name)) };

		TvecResult res = TVEC_PUSH(TrieResultEntry, array, entry);

		if(res != TvecResultOk) { // NOLINT(readability-implicit-bool-conversion)
			return TSTR_LIT("OOM in argument allocation");
		}

		current_branch = &(result->data.branch_and_value.branch);
	} else if(result->type == TrieNodeEntryTypeBranch) {
		current_branch = &(result->data.branch);
	} else {
		return TSTR_LIT("Invalid node entry type");
	}

	if(prefix.len == (current_depth + 1)) {
		// if we are the exact length, we need to add all entries from the branch, unconditionally

		return trie_add_matches_to_array_all_branch_impl(entries, array, current_branch);
	}

	// otherwise make more conditional adds, depending on the branch

	if(current_branch->is_trie_branch) {

		return trie_add_matches_to_array_with_prefix_impl(
		    entries, array, &(current_branch->data.trie.opaque->map), prefix, current_depth + 1);

	} else {

		return trie_add_matches_to_array_with_prefix_linear_impl(
		    entries, array, &(current_branch->data.linear), prefix);
	}
}

NODISCARD TrieSearchResult search_in_trie(const Trie* trie, tstr_view prefix) {

	if(prefix.len == 0) {

		// return all entries
		TrieResultEntryArray array = TVEC_EMPTY(TrieResultEntry);

		for(size_t i = 0; i < TVEC_LENGTH(TrieEntry, trie->array); ++i) {

			const TrieEntry* const arg = TVEC_GET_AT(TrieEntry, &(trie->array), i);
			ASSERT(arg != NULL);

			TrieResultEntry entry = { .name = tstr_as_view(&(arg->name)) };

			TvecResult res = TVEC_PUSH(TrieResultEntry, &array, entry);

			if(res != TvecResultOk) { // NOLINT(readability-implicit-bool-conversion)
				return (TrieSearchResult){
					.is_ok = false, .data = { .error = TSTR_LIT("OOM in argument allocation") }
				};
			}
		}

		return (TrieSearchResult){ .is_ok = true, .data = { .ok = array } };
	}

	TrieResultEntryArray array = TVEC_EMPTY(TrieResultEntry);

	tstr res = trie_add_matches_to_array_with_prefix_impl(&(trie->array), &array, &(trie->root.map),
	                                                      prefix, 0);

	if(!tstr_is_null(&res)) {
		return (TrieSearchResult){ .is_ok = false, .data = { .error = res } };
	}

	return (TrieSearchResult){ .is_ok = true, .data = { .ok = array } };
}

static void free_trie_hash_map(TrieNodeEntryHashMap* map);

static void free_trie_branch(TrieNodeEntryBranch* branch) {

	if(branch->is_trie_branch) {
		free_trie_hash_map(&(branch->data.trie.opaque->map));
		free(branch->data.trie.opaque);
	} else {
		TVEC_FREE(TrieNodeEntryValue, &(branch->data.linear.entries));
	}
}

static void free_trie_entry(TrieNodeEntry* entry) {

	if(entry->type == TrieNodeEntryTypeValue) {
		// Nothing to do
	} else if(entry->type == TrieNodeEntryTypeBranchAndValue) {
		free_trie_branch(&(entry->data.branch));
	} else if(entry->type == TrieNodeEntryTypeBranch) {
		free_trie_branch(&(entry->data.branch_and_value.branch));
	}
}

static void free_trie_hash_map(TrieNodeEntryHashMap* map) {
	TMAP_TYPENAME_ITER(TrieNodeEntryHashMap)
	iter = TMAP_ITER_INIT(TrieNodeEntryHashMap, map);

	TMAP_TYPENAME_ENTRY(TrieNodeEntryHashMap) value;

	while(TMAP_ITER_NEXT(TrieNodeEntryHashMap, &iter, &value)) {
		free_trie_entry(&value.value);
	}

	TMAP_FREE(TrieNodeEntryHashMap, map);
}

void free_trie(Trie* trie) {

	TVEC_FREE(TrieEntry, &(trie->array));

	free_trie_hash_map(&(trie->root.map));

	free(trie);
}

TMAP_IMPLEMENT_MAP_TYPE(TrieIdentifierType, TrieIdentifier, TrieNodeEntry, TrieNodeEntryHashMap)

TVEC_IMPLEMENT_VEC_TYPE(TrieResultEntry)
