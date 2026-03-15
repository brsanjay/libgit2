/*
 * Copyright (C) the libgit2 contributors. All rights reserved.
 *
 * This file is part of libgit2, distributed under the GNU GPL v2 with
 * a Linking Exception. For full terms see the included COPYING file.
 */
#ifndef INCLUDE_bundle_h__
#define INCLUDE_bundle_h__

#include "common.h"

#include "git2/bundle.h"
#include "git2/oid.h"
#include "str.h"
#include "vector.h"

/** Maximum number of ref or prerequisite entries accepted from a bundle header.
 *  Prevents memory exhaustion from maliciously crafted bundles. */
#define GIT_BUNDLE_MAX_REFS          65536
#define GIT_BUNDLE_MAX_PREREQUISITES 65536

/** A single prerequisite entry in a bundle. */
typedef struct {
	git_oid oid;
	char *comment;
} git_bundle_prerequisite;

/** A single reference entry in a bundle. */
typedef struct {
	git_oid oid;
	char *refname;
} git_bundle_ref;

struct git_bundle {
	int version;                /* 2 or 3 */
	git_oid_t oid_type;         /* GIT_OID_SHA1 for v2 */
	git_vector prerequisites;   /* git_bundle_prerequisite entries */
	git_vector refs;            /* git_bundle_ref entries */
	git_str path;               /* path to the bundle file on disk */
	size_t pack_offset;         /* byte offset of pack data in the file */
	size_t pack_len;            /* byte length of pack data */
	git_str raw;                /* header content only (pack data trimmed) */
};

#endif
