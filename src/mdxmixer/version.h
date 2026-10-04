#pragma once
// The one place the version number lives.
//
// Three NUMBERS, and the string is computed from them by stringification.
// MDropDX12 learned the cost of the other arrangement: its public-snapshot
// workflow scraped a literal out of version.h with a regex, and once the
// macro became computed there was no literal left to find -- so the regex
// matched the COMMENT instead and every public release was titled
// "MDropDX12 v// to be expanded before it is stringified, ...".
//
// The workflow here reads the three numbers by field, never the string, for
// exactly that reason. If you add a literal "0.1.0" anywhere below, you are
// re-creating the trap.
#define MDXM_VERSION_MAJOR 1
#define MDXM_VERSION_MINOR 0
#define MDXM_VERSION_PATCH 0

// Two steps: the inner macro has to be expanded before it is stringified,
// which a single-level macro would not do.
#define MDXM_STR_(x) #x
#define MDXM_STR(x) MDXM_STR_(x)
#define MDXM_VERSION_STR \
    MDXM_STR(MDXM_VERSION_MAJOR) "." MDXM_STR(MDXM_VERSION_MINOR) "." \
    MDXM_STR(MDXM_VERSION_PATCH)
