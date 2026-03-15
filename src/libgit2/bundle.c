/*
 * Copyright (C) the libgit2 contributors. All rights reserved.
 *
 * This file is part of libgit2, distributed under the GNU GPL v2 with
 * a Linking Exception. For full terms see the included COPYING file.
 */

#include "bundle.h"

#include "futils.h"
#include "parse.h"

#include "git2/odb.h"
#include "git2/odb_backend.h"
#include "git2/refs.h"
#include "git2/revwalk.h"
#include "git2/pack.h"
#include "git2/repository.h"
#include "git2/transaction.h"

#include "pack-objects.h"
#include "filebuf.h"

#define BUNDLE_V2_SIGNATURE "# v2 git bundle\n"
#define BUNDLE_V3_SIGNATURE "# v3 git bundle\n"

/* Streaming chunk size used when feeding pack data to the ODB writer. */
#define BUNDLE_PACK_CHUNK_SIZE 65536

static void free_prerequisite(git_bundle_prerequisite *prereq)
{
	if (prereq) {
		git__free(prereq->comment);
		git__free(prereq);
	}
}

static void free_ref(git_bundle_ref *ref)
{
	if (ref) {
		git__free(ref->refname);
		git__free(ref);
	}
}

static int parse_bundle_header(git_bundle *bundle)
{
	git_parse_ctx ctx = GIT_PARSE_CTX_INIT;
	int error;

	if ((error = git_parse_ctx_init(&ctx,
			bundle->raw.ptr, bundle->raw.size)) < 0)
		return error;

	/* Validate signature — accept v2 and v3 */
	if (git_parse_ctx_contains(&ctx,
			BUNDLE_V2_SIGNATURE,
			strlen(BUNDLE_V2_SIGNATURE))) {
		bundle->version = 2;
		bundle->oid_type = GIT_OID_SHA1;
		git_parse_advance_line(&ctx);
	} else if (git_parse_ctx_contains(&ctx,
			BUNDLE_V3_SIGNATURE,
			strlen(BUNDLE_V3_SIGNATURE))) {
		bundle->version = 3;
		bundle->oid_type = GIT_OID_SHA1; /* default; overridden by capabilities */
		git_parse_advance_line(&ctx);

		/*
		 * Parse v3 capability lines.  Each starts with '@'.
		 * We handle 'object-format' and silently skip everything
		 * else for forward compatibility.
		 */
		while (ctx.remain_len > 0) {
			const char *cap;
			size_t cap_len;
			char c;

			if (git_parse_peek(&c, &ctx, 0) < 0 || c != '@')
				break;

			git_parse_advance_chars(&ctx, 1); /* skip '@' */

			cap = ctx.line;
			cap_len = ctx.line_len;
			if (cap_len > 0 && cap[cap_len - 1] == '\n')
				cap_len--;

			if (cap_len == strlen("object-format=sha1") &&
			    memcmp(cap, "object-format=sha1",
				    strlen("object-format=sha1")) == 0) {
				/* explicit SHA-1 — already the default */
			} else if (cap_len >= strlen("object-format=sha256") &&
			    memcmp(cap, "object-format=sha256",
				    strlen("object-format=sha256")) == 0) {
				git_error_set(GIT_ERROR_BUNDLE,
					"bundle uses SHA-256 object format "
					"which is not yet supported");
				return GIT_ENOTSUPPORTED;
			}
			/* all other capabilities are ignored */

			git_parse_advance_line(&ctx);
		}
	} else {
		git_error_set(GIT_ERROR_BUNDLE,
			"invalid bundle: unrecognised signature "
			"(expected '# v2 git bundle' or '# v3 git bundle')");
		return -1;
	}

	/* Parse prerequisites and references.
	 * Prerequisites start with '-', references are hex OIDs,
	 * a blank line separates the header from pack data. */
	while (ctx.remain_len > 0) {
		char c;

		if (git_parse_peek(&c, &ctx, 0) < 0)
			break;

		/* Blank line: end of header */
		if (c == '\n') {
			git_parse_advance_line(&ctx);
			break;
		}

		/* Prerequisite line: -<oid> <comment>\n */
		if (c == '-') {
			git_bundle_prerequisite *prereq;
			const char *line_start;
			size_t line_len;

			/* Fix #1: cap entries to prevent memory exhaustion */
			if (bundle->prerequisites.length >=
					GIT_BUNDLE_MAX_PREREQUISITES) {
				git_error_set(GIT_ERROR_BUNDLE,
					"invalid bundle: too many prerequisites "
					"(max %d)", GIT_BUNDLE_MAX_PREREQUISITES);
				return -1;
			}

			git_parse_advance_chars(&ctx, 1); /* skip '-' */

			prereq = git__calloc(1, sizeof(*prereq));
			GIT_ERROR_CHECK_ALLOC(prereq);

			if (git_parse_advance_oid(&prereq->oid, &ctx,
					GIT_OID_SHA1) < 0) {
				git_error_set(GIT_ERROR_BUNDLE,
					"invalid bundle: bad prerequisite OID "
					"at line %"PRIuZ, ctx.line_num);
				free_prerequisite(prereq);
				return -1;
			}

			/* Remainder of the line is the comment */
			line_start = ctx.line;
			line_len = ctx.line_len;

			/* Skip leading space if present */
			if (line_len > 0 && *line_start == ' ') {
				line_start++;
				line_len--;
			}

			/* Strip trailing newline */
			if (line_len > 0 && line_start[line_len - 1] == '\n')
				line_len--;

			if (line_len > 0) {
				prereq->comment = git__strndup(
					line_start, line_len);
				GIT_ERROR_CHECK_ALLOC(prereq->comment);
			}

			if ((error = git_vector_insert(
					&bundle->prerequisites, prereq)) < 0) {
				free_prerequisite(prereq);
				return error;
			}

			git_parse_advance_line(&ctx);
			continue;
		}

		/* Reference line: <oid> <refname>\n */
		{
			git_bundle_ref *ref;
			const char *name_start;
			size_t name_len;
			int valid;

			/* Fix #1: cap entries to prevent memory exhaustion */
			if (bundle->refs.length >= GIT_BUNDLE_MAX_REFS) {
				git_error_set(GIT_ERROR_BUNDLE,
					"invalid bundle: too many refs "
					"(max %d)", GIT_BUNDLE_MAX_REFS);
				return -1;
			}

			ref = git__calloc(1, sizeof(*ref));
			GIT_ERROR_CHECK_ALLOC(ref);

			if (git_parse_advance_oid(&ref->oid, &ctx,
					GIT_OID_SHA1) < 0) {
				git_error_set(GIT_ERROR_BUNDLE,
					"invalid bundle: bad reference OID "
					"at line %"PRIuZ, ctx.line_num);
				free_ref(ref);
				return -1;
			}

			/* Skip the space */
			if (git_parse_advance_ws(&ctx) < 0) {
				git_error_set(GIT_ERROR_BUNDLE,
					"invalid bundle: expected space after "
					"OID at line %"PRIuZ, ctx.line_num);
				free_ref(ref);
				return -1;
			}

			/* Remainder is the refname */
			name_start = ctx.line;
			name_len = ctx.line_len;

			/* Strip trailing newline */
			if (name_len > 0 &&
					name_start[name_len - 1] == '\n')
				name_len--;

			if (name_len == 0) {
				git_error_set(GIT_ERROR_BUNDLE,
					"invalid bundle: empty reference name "
					"at line %"PRIuZ, ctx.line_num);
				free_ref(ref);
				return -1;
			}

			ref->refname = git__strndup(name_start, name_len);
			GIT_ERROR_CHECK_ALLOC(ref->refname);

			/* Fix #6: validate refname from untrusted bundle
			 * content before it can reach ref-creation or
			 * filesystem code */
			if (git_reference_name_is_valid(&valid,
					ref->refname) < 0 || !valid) {
				git_error_set(GIT_ERROR_BUNDLE,
					"invalid bundle: invalid reference "
					"name '%s' at line %"PRIuZ,
					ref->refname, ctx.line_num);
				free_ref(ref);
				return -1;
			}

			if ((error = git_vector_insert(
					&bundle->refs, ref)) < 0) {
				free_ref(ref);
				return error;
			}

			git_parse_advance_line(&ctx);
		}
	}

	/* Record offset and length of pack data within the file */
	bundle->pack_offset = (size_t)(ctx.line - ctx.content);
	bundle->pack_len = ctx.remain_len;

	return 0;
}

int git_bundle_open(git_bundle **out, const char *path)
{
	git_bundle *bundle;
	int error;

	GIT_ASSERT_ARG(out);
	GIT_ASSERT_ARG(path);

	*out = NULL;

	bundle = git__calloc(1, sizeof(*bundle));
	GIT_ERROR_CHECK_ALLOC(bundle);

	/* Fix #2: store path so pack data can be streamed on demand
	 * rather than holding the entire bundle in memory */
	if ((error = git_str_puts(&bundle->path, path)) < 0)
		goto on_error;

	if ((error = git_futils_readbuffer(&bundle->raw, path)) < 0)
		goto on_error;

	if ((error = git_vector_init(&bundle->prerequisites, 0, NULL)) < 0)
		goto on_error;

	if ((error = git_vector_init(&bundle->refs, 4, NULL)) < 0)
		goto on_error;

	if ((error = parse_bundle_header(bundle)) < 0)
		goto on_error;

	/* Fix #2: trim the raw buffer to the header only.  Pack data
	 * starts at pack_offset and is streamed directly from the file
	 * in git_bundle_unbundle, so we do not need to keep it in RAM. */
	git_str_truncate(&bundle->raw, bundle->pack_offset);

	*out = bundle;
	return 0;

on_error:
	git_bundle_free(bundle);
	return error;
}

void git_bundle_free(git_bundle *bundle)
{
	size_t i;

	if (!bundle)
		return;

	for (i = 0; i < bundle->prerequisites.length; i++)
		free_prerequisite(git_vector_get(&bundle->prerequisites, i));
	git_vector_dispose(&bundle->prerequisites);

	for (i = 0; i < bundle->refs.length; i++)
		free_ref(git_vector_get(&bundle->refs, i));
	git_vector_dispose(&bundle->refs);

	git_str_dispose(&bundle->path);
	git_str_dispose(&bundle->raw);
	git__free(bundle);
}

size_t git_bundle_ref_count(git_bundle *bundle)
{
	GIT_ASSERT_ARG_WITH_RETVAL(bundle, 0);
	return bundle->refs.length;
}

int git_bundle_ref_byindex(
	const git_oid **oid_out,
	const char **refname_out,
	git_bundle *bundle,
	size_t idx)
{
	git_bundle_ref *ref;

	GIT_ASSERT_ARG(bundle);

	if (idx >= bundle->refs.length) {
		git_error_set(GIT_ERROR_BUNDLE,
			"bundle ref index %"PRIuZ" out of range", idx);
		return GIT_ENOTFOUND;
	}

	ref = git_vector_get(&bundle->refs, idx);

	if (oid_out)
		*oid_out = &ref->oid;
	if (refname_out)
		*refname_out = ref->refname;

	return 0;
}

size_t git_bundle_prerequisite_count(git_bundle *bundle)
{
	GIT_ASSERT_ARG_WITH_RETVAL(bundle, 0);
	return bundle->prerequisites.length;
}

int git_bundle_prerequisite_byindex(
	const git_oid **oid_out,
	const char **comment_out,
	git_bundle *bundle,
	size_t idx)
{
	git_bundle_prerequisite *prereq;

	GIT_ASSERT_ARG(bundle);

	if (idx >= bundle->prerequisites.length) {
		git_error_set(GIT_ERROR_BUNDLE,
			"bundle prerequisite index %"PRIuZ
			" out of range", idx);
		return GIT_ENOTFOUND;
	}

	prereq = git_vector_get(&bundle->prerequisites, idx);

	if (oid_out)
		*oid_out = &prereq->oid;
	if (comment_out)
		*comment_out = prereq->comment;

	return 0;
}

int git_bundle_verify(
	git_bundle *bundle,
	git_repository *repo)
{
	git_odb *odb = NULL;
	size_t i;
	int error;

	GIT_ASSERT_ARG(bundle);
	GIT_ASSERT_ARG(repo);

	if ((error = git_repository_odb(&odb, repo)) < 0)
		return error;

	for (i = 0; i < bundle->prerequisites.length; i++) {
		git_bundle_prerequisite *prereq =
			git_vector_get(&bundle->prerequisites, i);
		char oid_str[GIT_OID_SHA1_HEXSIZE + 1];

		if (!git_odb_exists(odb, &prereq->oid)) {
			/* Fix #7: use local buffer instead of thread-local
			 * git_oid_tostr_s to avoid fragile TLS aliasing */
			git_oid_tostr(oid_str, sizeof(oid_str), &prereq->oid);
			git_error_set(GIT_ERROR_BUNDLE,
				"prerequisite %s is missing", oid_str);
			git_odb_free(odb);
			return GIT_ENOTFOUND;
		}
	}

	git_odb_free(odb);
	return 0;
}

int git_bundle_create_options_init(
	git_bundle_create_options *opts,
	unsigned int version)
{
	GIT_INIT_STRUCTURE_FROM_TEMPLATE(
		opts, version, git_bundle_create_options,
		GIT_BUNDLE_CREATE_OPTIONS_INIT);
	return 0;
}

int git_bundle_unbundle_options_init(
	git_bundle_unbundle_options *opts,
	unsigned int version)
{
	GIT_INIT_STRUCTURE_FROM_TEMPLATE(
		opts, version, git_bundle_unbundle_options,
		GIT_BUNDLE_UNBUNDLE_OPTIONS_INIT);
	return 0;
}

int git_bundle_create(
	const char *path,
	git_repository *repo,
	const char **refnames,
	size_t refnames_count,
	const git_oid *prerequisites,
	size_t prerequisites_count,
	const git_bundle_create_options *opts)
{
	git_str header = GIT_STR_INIT;
	git_str pack = GIT_STR_INIT;
	git_packbuilder *pb = NULL;
	git_revwalk *walk = NULL;
	git_filebuf file = GIT_FILEBUF_INIT;
	git_oid *ref_oids = NULL;
	size_t i;
	int error;

	GIT_UNUSED(opts);

	GIT_ASSERT_ARG(path);
	GIT_ASSERT_ARG(repo);

	if (refnames_count == 0 || !refnames) {
		git_error_set(GIT_ERROR_BUNDLE,
			"at least one reference is required");
		return -1;
	}

	/* Fix #3: resolve all ref OIDs upfront to eliminate the TOCTOU
	 * window between header writing and revwalk setup.  Both phases
	 * now use the same snapshot of OIDs. */
	ref_oids = git__calloc(refnames_count, sizeof(git_oid));
	GIT_ERROR_CHECK_ALLOC(ref_oids);

	for (i = 0; i < refnames_count; i++) {
		if ((error = git_reference_name_to_id(
				&ref_oids[i], repo, refnames[i])) < 0)
			goto cleanup;
	}

	/* Build the text header */
	if ((error = git_str_puts(&header, BUNDLE_V2_SIGNATURE)) < 0)
		goto cleanup;

	/* Write prerequisite lines */
	for (i = 0; i < prerequisites_count; i++) {
		/* Fix #7: use local buffer instead of thread-local
		 * git_oid_tostr_s to avoid fragile TLS aliasing */
		char oid_str[GIT_OID_SHA1_HEXSIZE + 1];
		git_oid_tostr(oid_str, sizeof(oid_str), &prerequisites[i]);

		if ((error = git_str_putc(&header, '-')) < 0 ||
		    (error = git_str_puts(&header, oid_str)) < 0 ||
		    (error = git_str_putc(&header, '\n')) < 0)
			goto cleanup;
	}

	/* Write reference lines using the pre-resolved OIDs */
	for (i = 0; i < refnames_count; i++) {
		char oid_str[GIT_OID_SHA1_HEXSIZE + 1];
		git_oid_tostr(oid_str, sizeof(oid_str), &ref_oids[i]);

		if ((error = git_str_puts(&header, oid_str)) < 0 ||
		    (error = git_str_putc(&header, ' ')) < 0 ||
		    (error = git_str_puts(&header, refnames[i])) < 0 ||
		    (error = git_str_putc(&header, '\n')) < 0)
			goto cleanup;
	}

	/* Blank line separates header from pack data */
	if ((error = git_str_putc(&header, '\n')) < 0)
		goto cleanup;

	/* Build the packfile */
	if ((error = git_packbuilder_new(&pb, repo)) < 0 ||
	    (error = git_revwalk_new(&walk, repo)) < 0)
		goto cleanup;

	/* Push the same pre-resolved OIDs into the revwalk */
	for (i = 0; i < refnames_count; i++) {
		if ((error = git_revwalk_push(walk, &ref_oids[i])) < 0)
			goto cleanup;
	}

	/* Hide prerequisite OIDs */
	for (i = 0; i < prerequisites_count; i++) {
		if ((error = git_revwalk_hide(
				walk, &prerequisites[i])) < 0)
			goto cleanup;
	}

	/* Fix annotated tags: git_packbuilder_insert_walk only follows
	 * commits (revwalk peels tag refs to their underlying commit).
	 * Use insert_recur on each ref tip so that annotated tag objects
	 * — and any multi-level tag chains — are included in the pack.
	 * For plain commit refs this is a no-op (packbuilder deduplicates). */
	for (i = 0; i < refnames_count; i++) {
		if ((error = git_packbuilder_insert_recur(
				pb, &ref_oids[i], refnames[i])) < 0)
			goto cleanup;
	}

	if ((error = git_packbuilder_insert_walk(pb, walk)) < 0 ||
	    (error = git_packbuilder__write_buf(&pack, pb)) < 0)
		goto cleanup;

	/* Write header + pack atomically via filebuf */
	if ((error = git_filebuf_open(
			&file, path, 0, 0666)) < 0 ||
	    (error = git_filebuf_write(
			&file, header.ptr, header.size)) < 0 ||
	    (error = git_filebuf_write(
			&file, pack.ptr, pack.size)) < 0 ||
	    (error = git_filebuf_commit(&file)) < 0)
		goto cleanup;

cleanup:
	if (error < 0)
		git_filebuf_cleanup(&file);

	git__free(ref_oids);
	git_revwalk_free(walk);
	git_packbuilder_free(pb);
	git_str_dispose(&pack);
	git_str_dispose(&header);

	return error;
}

int git_bundle_unbundle(
	git_repository *repo,
	git_bundle *bundle,
	git_indexer_progress_cb progress_cb,
	void *progress_cb_payload,
	const git_bundle_unbundle_options *opts)
{
	git_odb *odb = NULL;
	git_odb_writepack *writepack = NULL;
	git_transaction *tx = NULL;
	git_indexer_progress stats = {0};
	int force_update = opts ? opts->force_update_refs : 0;
	git_file fd = -1;
	size_t i;
	int error;

	GIT_ASSERT_ARG(repo);
	GIT_ASSERT_ARG(bundle);

	/* Verify prerequisites first */
	if ((error = git_bundle_verify(bundle, repo)) < 0)
		return error;

	/* Fix #4: pre-validate ref conflicts before any ODB writes so
	 * that we fail cleanly with no partial state when force is off */
	if (!force_update) {
		for (i = 0; i < bundle->refs.length; i++) {
			git_bundle_ref *ref =
				git_vector_get(&bundle->refs, i);
			git_reference *existing = NULL;
			int rc = git_reference_lookup(
				&existing, repo, ref->refname);
			git_reference_free(existing);

			if (rc == 0) {
				git_error_set(GIT_ERROR_BUNDLE,
					"ref '%s' already exists; set "
					"force_update_refs to overwrite",
					ref->refname);
				return GIT_EEXISTS;
			}
			/* GIT_ENOTFOUND is expected (ref doesn't exist yet) */
			if (rc != GIT_ENOTFOUND)
				return rc;
		}
	}

	if ((error = git_repository_odb(&odb, repo)) < 0)
		return error;

	/* Fix #2: stream pack data from file in chunks rather than
	 * holding it all in memory */
	if ((error = git_odb_write_pack(
			&writepack, odb,
			progress_cb, progress_cb_payload)) < 0)
		goto cleanup;

	if (bundle->pack_len > 0) {
		char buf[BUNDLE_PACK_CHUNK_SIZE];
		size_t remaining = bundle->pack_len;

		if ((fd = git_futils_open_ro(bundle->path.ptr)) < 0) {
			error = fd;
			goto cleanup;
		}

		if (p_lseek(fd, (off_t)bundle->pack_offset,
				SEEK_SET) < 0) {
			git_error_set(GIT_ERROR_OS,
				"could not seek to pack data in bundle");
			error = -1;
			goto cleanup;
		}

		while (remaining > 0) {
			size_t chunk = remaining < sizeof(buf)
				? remaining : sizeof(buf);
			ssize_t n = p_read(fd, buf, chunk);

			if (n <= 0) {
				git_error_set(GIT_ERROR_OS,
					"error reading pack data "
					"from bundle");
				error = -1;
				goto cleanup;
			}

			if ((error = writepack->append(
					writepack, buf,
					(size_t)n, &stats)) < 0)
				goto cleanup;

			remaining -= (size_t)n;
		}
	}

	if ((error = writepack->commit(writepack, &stats)) < 0)
		goto cleanup;

	/* Fix #5: apply all ref updates atomically via a transaction.
	 * If any lock or set_target call fails, git_transaction_free
	 * releases all locks without committing, leaving no partial
	 * ref state in the repository. */
	if ((error = git_transaction_new(&tx, repo)) < 0)
		goto cleanup;

	for (i = 0; i < bundle->refs.length; i++) {
		git_bundle_ref *ref = git_vector_get(&bundle->refs, i);

		if ((error = git_transaction_lock_ref(
				tx, ref->refname)) < 0 ||
		    (error = git_transaction_set_target(
				tx, ref->refname, &ref->oid,
				NULL, "bundle unbundle")) < 0)
			goto cleanup;
	}

	error = git_transaction_commit(tx);

cleanup:
	if (fd >= 0)
		p_close(fd);
	if (tx)
		git_transaction_free(tx);
	if (writepack)
		writepack->free(writepack);
	git_odb_free(odb);

	return error;
}
