#pragma once

#define RETURNS_VOID (void)

#define IGNORE_RESULT (void)

#ifdef __cplusplus
extern "C" {
#endif

// see
// https://clang.llvm.org/docs/AttributeReference.html#nullability-attributes

#if defined(__GNUC__) || defined(__clang__)
#if defined(__clang__)
#define NULLABLE _Nullable
#define NON_NULLABLE _Nonnull
#define NO_NULLABLE_INFO _Null_unspecified
#else
#define NULLABLE
#define NON_NULLABLE
#define NO_NULLABLE_INFO
#endif

#define OUT_PARAM(type) type *NON_NULLABLE
#define DEFAULT_PARAM(type) type *NO_NULLABLE_INFO
#elif defined(_MSC_VER)
#define NULLABLE
#define NON_NULLABLE
#define NO_NULLABLE_INFO

#define OUT_PARAM(type) _Out_ type *
#define DEFAULT_PARAM(type) type *

#else
#define NULLABLE
#define NON_NULLABLE
#define NO_NULLABLE_INFO

#define OUT_PARAM(type) type *
#define DEFAULT_PARAM(type) type *
#endif

#ifdef __cplusplus
}
#endif

#ifdef __cplusplus
extern "C" {
#endif

#if (defined(__STDC_VERSION__) && __STDC_VERSION__ >= 202000) || __cplusplus
#define NODISCARD [[nodiscard]]
#define MAYBE_UNUSED [[maybe_unused]]
#else
// see e.g. https://www.gnu.org/software/gnulib/manual/html_node/Attributes.html
#define NODISCARD __attribute__((__warn_unused_result__))
#define MAYBE_UNUSED __attribute__((__unused__))
#endif

#define UNUSED(v) ((void)(v))

#ifdef __cplusplus
}
#endif

#define ARRAY_SIZE(arr) (sizeof(arr) / sizeof(*arr))
