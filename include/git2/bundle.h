/*
 * Copyright (C) the libgit2 contributors. All rights reserved.
 *
 * This file is part of libgit2, distributed under the GNU GPL v2 with
 * a Linking Exception. For full terms see the included COPYING file.
 */
#ifndef INCLUDE_git_bundle_h__
#define INCLUDE_git_bundle_h__

#include "common.h"
#include "types.h"
#include "oid.h"
#include "indexer.h"

/**
 * @file git2/bundle.h
 * @brief Git bundle support
 *
 * Git bundles are self-contained files that package repository refs
 * and objects (as a packfile), enabling offline transfer of repository
 * history.
 *
 * @defgroup git_bundle Git bundle routines
 * @ingroup Git
 * @{
 */
GIT_BEGIN_DECL

/**
 * Options for creating a bundle.
 */
typedef struct git_bundle_create_options {
	unsigned int version;
} git_bundle_create_options;

#define GIT_BUNDLE_CREATE_OPTIONS_VERSION 1
#define GIT_BUNDLE_CREATE_OPTIONS_INIT { GIT_BUNDLE_CREATE_OPTIONS_VERSION }

/**
 * Initialize git_bundle_create_options structure.
 *
 * Initializes a `git_bundle_create_options` with default values.
 * Equivalent to creating an instance with
 * `GIT_BUNDLE_CREATE_OPTIONS_INIT`.
 *
 * @param opts The `git_bundle_create_options` struct to initialize.
 * @param version The struct version; pass `GIT_BUNDLE_CREATE_OPTIONS_VERSION`.
 * @return 0 on success or -1 on failure.
 */
GIT_EXTERN(int) git_bundle_create_options_init(
	git_bundle_create_options *opts,
	unsigned int version);

/**
 * Options for unbundling (importing) a bundle.
 */
typedef struct git_bundle_unbundle_options {
	unsigned int version;

	/**
	 * If non-zero, overwrite any existing refs in the target repository
	 * that conflict with refs in the bundle.  The default (0) is to
	 * return GIT_EEXISTS when a ref already exists, leaving the
	 * repository unmodified.
	 */
	int force_update_refs;
} git_bundle_unbundle_options;

#define GIT_BUNDLE_UNBUNDLE_OPTIONS_VERSION 1
#define GIT_BUNDLE_UNBUNDLE_OPTIONS_INIT \
	{ GIT_BUNDLE_UNBUNDLE_OPTIONS_VERSION, 0 }

/**
 * Initialize git_bundle_unbundle_options structure.
 *
 * Initializes a `git_bundle_unbundle_options` with default values.
 * Equivalent to creating an instance with
 * `GIT_BUNDLE_UNBUNDLE_OPTIONS_INIT`.
 *
 * @param opts The `git_bundle_unbundle_options` struct to initialize.
 * @param version The struct version; pass
 *        `GIT_BUNDLE_UNBUNDLE_OPTIONS_VERSION`.
 * @return 0 on success or -1 on failure.
 */
GIT_EXTERN(int) git_bundle_unbundle_options_init(
	git_bundle_unbundle_options *opts,
	unsigned int version);

/**
 * Open and parse a bundle file.
 *
 * Reads the bundle file at the given path and parses its header
 * (signature, prerequisites, references) and records the location
 * of the embedded packfile data.  Only the header is kept in memory;
 * pack data is streamed from the file on demand during unbundle.
 *
 * @param out Pointer to store the newly allocated bundle object.
 * @param path Path to the bundle file on disk.
 * @return 0 on success, or an error code.
 */
GIT_EXTERN(int) git_bundle_open(git_bundle **out, const char *path);

/**
 * Free a bundle object and all associated resources.
 *
 * @param bundle The bundle to free; may be NULL.
 */
GIT_EXTERN(void) git_bundle_free(git_bundle *bundle);

/**
 * Get the number of references in the bundle.
 *
 * @param bundle The bundle.
 * @return The number of references.
 */
GIT_EXTERN(size_t) git_bundle_ref_count(git_bundle *bundle);

/**
 * Get a reference from the bundle by index.
 *
 * The returned OID and refname pointers are valid as long as the
 * bundle is not freed.
 *
 * @param oid_out Pointer to store the reference's OID; may be NULL.
 * @param refname_out Pointer to store the reference name; may be NULL.
 * @param bundle The bundle.
 * @param idx The index of the reference (0-based).
 * @return 0 on success, GIT_ENOTFOUND if index is out of bounds.
 */
GIT_EXTERN(int) git_bundle_ref_byindex(
	const git_oid **oid_out,
	const char **refname_out,
	git_bundle *bundle,
	size_t idx);

/**
 * Get the number of prerequisites in the bundle.
 *
 * @param bundle The bundle.
 * @return The number of prerequisites.
 */
GIT_EXTERN(size_t) git_bundle_prerequisite_count(git_bundle *bundle);

/**
 * Get a prerequisite from the bundle by index.
 *
 * The returned OID and comment pointers are valid as long as the
 * bundle is not freed.
 *
 * @param oid_out Pointer to store the prerequisite's OID; may be NULL.
 * @param comment_out Pointer to store the prerequisite comment; may be NULL.
 * @param bundle The bundle.
 * @param idx The index of the prerequisite (0-based).
 * @return 0 on success, GIT_ENOTFOUND if index is out of bounds.
 */
GIT_EXTERN(int) git_bundle_prerequisite_byindex(
	const git_oid **oid_out,
	const char **comment_out,
	git_bundle *bundle,
	size_t idx);

/**
 * Verify a bundle against a repository.
 *
 * Checks that all prerequisites listed in the bundle exist in the
 * repository's object database.
 *
 * @param bundle The bundle to verify.
 * @param repo The repository to check against.
 * @return 0 if all prerequisites are present, GIT_ENOTFOUND if
 *         a prerequisite is missing.
 */
GIT_EXTERN(int) git_bundle_verify(
	git_bundle *bundle,
	git_repository *repo);

/**
 * Create a bundle file from a repository.
 *
 * Writes a V2 bundle file containing the specified references and
 * an embedded packfile with all objects reachable from those
 * references, excluding objects reachable from the prerequisites.
 *
 * @param path Path where the bundle file should be written.
 * @param repo The source repository.
 * @param refnames Array of reference names to include.
 * @param refnames_count Number of entries in refnames.
 * @param prerequisites Array of prerequisite OIDs; may be NULL.
 * @param prerequisites_count Number of entries in prerequisites.
 * @param opts Options for bundle creation; may be NULL.
 * @return 0 on success, or an error code.
 */
GIT_EXTERN(int) git_bundle_create(
	const char *path,
	git_repository *repo,
	const char **refnames,
	size_t refnames_count,
	const git_oid *prerequisites,
	size_t prerequisites_count,
	const git_bundle_create_options *opts);

/**
 * Unbundle (import) a bundle into a repository.
 *
 * Verifies that all prerequisites exist, then streams the embedded
 * packfile into the repository's object database and creates or
 * updates references for each ref in the bundle.  All ref updates
 * are applied atomically: if any ref fails the entire set is rolled
 * back.
 *
 * @param repo The target repository.
 * @param bundle The bundle to import.
 * @param progress_cb Optional progress callback; may be NULL.
 * @param progress_cb_payload Payload for the progress callback.
 * @param opts Options controlling ref-update behaviour; may be NULL
 *        to use defaults (no force overwrite).
 * @return 0 on success, GIT_EEXISTS if a ref already exists and
 *         force_update_refs is not set, or another error code.
 */
GIT_EXTERN(int) git_bundle_unbundle(
	git_repository *repo,
	git_bundle *bundle,
	git_indexer_progress_cb progress_cb,
	void *progress_cb_payload,
	const git_bundle_unbundle_options *opts);

/** @} */
GIT_END_DECL

#endif
