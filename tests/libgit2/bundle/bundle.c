#include "clar_libgit2.h"

#include <git2.h>
#include "git2/bundle.h"
#include "bundle.h"
#include "futils.h"

static git_repository *_repo = NULL;

void test_bundle_bundle__initialize(void)
{
	cl_git_pass(git_repository_open(&_repo,
		cl_fixture("testrepo.git")));
}

void test_bundle_bundle__cleanup(void)
{
	git_repository_free(_repo);
	_repo = NULL;
	cl_fixture_cleanup("clone.git");
}

void test_bundle_bundle__open_valid(void)
{
	git_bundle *bundle = NULL;

	cl_git_pass(git_bundle_open(&bundle,
		cl_fixture("bundle.v2")));
	cl_assert(bundle != NULL);
	cl_assert(git_bundle_ref_count(bundle) > 0);

	git_bundle_free(bundle);
}

void test_bundle_bundle__reject_invalid_signature(void)
{
	git_bundle *bundle = NULL;
	git_str path = GIT_STR_INIT;
	git_str content = GIT_STR_INIT;

	/* Write a file with an invalid signature */
	cl_git_pass(git_str_joinpath(&path,
		clar_sandbox_path(), "bad.bundle"));
	cl_git_pass(git_str_puts(&content,
		"# not a bundle\ngarbage\n"));
	cl_git_pass(git_futils_writebuffer(
		&content, path.ptr, O_WRONLY | O_CREAT | O_TRUNC, 0644));

	cl_git_fail(git_bundle_open(&bundle, path.ptr));
	cl_assert(bundle == NULL);

	git_str_dispose(&path);
	git_str_dispose(&content);
}

void test_bundle_bundle__enumerate_refs(void)
{
	git_bundle *bundle = NULL;
	size_t count, i;

	cl_git_pass(git_bundle_open(&bundle,
		cl_fixture("bundle.v2")));

	count = git_bundle_ref_count(bundle);
	cl_assert(count > 0);

	for (i = 0; i < count; i++) {
		const git_oid *oid = NULL;
		const char *refname = NULL;

		cl_git_pass(git_bundle_ref_byindex(
			&oid, &refname, bundle, i));
		cl_assert(oid != NULL);
		cl_assert(refname != NULL);
		cl_assert(strlen(refname) > 0);
		cl_assert(!git_oid_is_zero(oid));
	}

	git_bundle_free(bundle);
}

void test_bundle_bundle__ref_bounds_check(void)
{
	git_bundle *bundle = NULL;
	const git_oid *oid = NULL;
	const char *refname = NULL;
	size_t count;

	cl_git_pass(git_bundle_open(&bundle,
		cl_fixture("bundle.v2")));

	count = git_bundle_ref_count(bundle);
	cl_assert_equal_i(GIT_ENOTFOUND,
		git_bundle_ref_byindex(&oid, &refname, bundle, count));
	cl_assert_equal_i(GIT_ENOTFOUND,
		git_bundle_ref_byindex(&oid, &refname, bundle, count + 100));

	git_bundle_free(bundle);
}

void test_bundle_bundle__enumerate_prerequisites(void)
{
	git_bundle *bundle = NULL;
	size_t count, i;

	cl_git_pass(git_bundle_open(&bundle,
		cl_fixture("bundle_incremental.v2")));

	count = git_bundle_prerequisite_count(bundle);
	cl_assert(count > 0);

	for (i = 0; i < count; i++) {
		const git_oid *oid = NULL;
		const char *comment = NULL;

		cl_git_pass(git_bundle_prerequisite_byindex(
			&oid, &comment, bundle, i));
		cl_assert(oid != NULL);
		cl_assert(!git_oid_is_zero(oid));
	}

	git_bundle_free(bundle);
}

void test_bundle_bundle__prerequisite_bounds_check(void)
{
	git_bundle *bundle = NULL;
	const git_oid *oid = NULL;
	const char *comment = NULL;
	size_t count;

	cl_git_pass(git_bundle_open(&bundle,
		cl_fixture("bundle_incremental.v2")));

	count = git_bundle_prerequisite_count(bundle);
	cl_assert_equal_i(GIT_ENOTFOUND,
		git_bundle_prerequisite_byindex(
			&oid, &comment, bundle, count));

	git_bundle_free(bundle);
}

void test_bundle_bundle__verify_full_bundle(void)
{
	git_bundle *bundle = NULL;

	cl_git_pass(git_bundle_open(&bundle,
		cl_fixture("bundle.v2")));

	/* A full bundle (no prerequisites) should always verify */
	cl_git_pass(git_bundle_verify(bundle, _repo));

	git_bundle_free(bundle);
}

void test_bundle_bundle__verify_incremental_succeeds(void)
{
	git_bundle *bundle = NULL;

	cl_git_pass(git_bundle_open(&bundle,
		cl_fixture("bundle_incremental.v2")));

	/* testrepo.git should have all prerequisites */
	cl_git_pass(git_bundle_verify(bundle, _repo));

	git_bundle_free(bundle);
}

void test_bundle_bundle__verify_missing_prereq_fails(void)
{
	git_bundle *bundle = NULL;
	git_repository *empty_repo = NULL;
	git_str path = GIT_STR_INIT;

	cl_git_pass(git_bundle_open(&bundle,
		cl_fixture("bundle_incremental.v2")));

	/* Create an empty repo that won't have the prerequisites */
	cl_git_pass(git_str_joinpath(&path,
		clar_sandbox_path(), "empty.git"));
	cl_git_pass(git_repository_init(&empty_repo, path.ptr, 1));

	cl_assert_equal_i(GIT_ENOTFOUND,
		git_bundle_verify(bundle, empty_repo));

	git_repository_free(empty_repo);
	cl_fixture_cleanup("empty.git");
	git_str_dispose(&path);
	git_bundle_free(bundle);
}

void test_bundle_bundle__create_and_reparse(void)
{
	git_bundle *bundle = NULL;
	git_str path = GIT_STR_INIT;
	const char *refnames[] = { "refs/heads/master" };
	const git_oid *oid;
	const char *refname;

	cl_git_pass(git_str_joinpath(&path,
		clar_sandbox_path(), "output.bundle"));

	cl_git_pass(git_bundle_create(
		path.ptr, _repo, refnames, 1, NULL, 0, NULL));

	/* Parse it back */
	cl_git_pass(git_bundle_open(&bundle, path.ptr));
	cl_assert_equal_sz(1, git_bundle_ref_count(bundle));
	cl_assert_equal_sz(0, git_bundle_prerequisite_count(bundle));

	cl_git_pass(git_bundle_ref_byindex(
		&oid, &refname, bundle, 0));
	cl_assert_equal_s("refs/heads/master", refname);

	git_bundle_free(bundle);
	git_str_dispose(&path);
}

void test_bundle_bundle__create_with_prerequisites(void)
{
	git_bundle *bundle = NULL;
	git_str path = GIT_STR_INIT;
	git_oid prereq_oid;
	const char *refnames[] = { "refs/heads/master" };

	cl_git_pass(git_oid_fromstr(&prereq_oid,
		"8496071c1b46c854b31185ea97743be6a8774479"));

	cl_git_pass(git_str_joinpath(&path,
		clar_sandbox_path(), "output_prereq.bundle"));

	cl_git_pass(git_bundle_create(
		path.ptr, _repo, refnames, 1, &prereq_oid, 1, NULL));

	/* Parse it back */
	cl_git_pass(git_bundle_open(&bundle, path.ptr));
	cl_assert_equal_sz(1, git_bundle_ref_count(bundle));
	cl_assert_equal_sz(1, git_bundle_prerequisite_count(bundle));

	git_bundle_free(bundle);
	git_str_dispose(&path);
}

void test_bundle_bundle__create_no_refs_fails(void)
{
	git_str path = GIT_STR_INIT;

	cl_git_pass(git_str_joinpath(&path,
		clar_sandbox_path(), "empty.bundle"));

	cl_git_fail(git_bundle_create(
		path.ptr, _repo, NULL, 0, NULL, 0, NULL));

	git_str_dispose(&path);
}

void test_bundle_bundle__unbundle_full(void)
{
	git_bundle *bundle = NULL;
	git_repository *clone_repo = NULL;
	git_str path = GIT_STR_INIT;
	git_str bundle_path = GIT_STR_INIT;
	const char *refnames[] = { "refs/heads/master" };
	git_oid original_oid, unbundled_oid;

	/* Get the OID of master in the source repo */
	cl_git_pass(git_reference_name_to_id(
		&original_oid, _repo, "refs/heads/master"));

	/* Create a bundle from the source repo */
	cl_git_pass(git_str_joinpath(&bundle_path,
		clar_sandbox_path(), "roundtrip.bundle"));
	cl_git_pass(git_bundle_create(
		bundle_path.ptr, _repo, refnames, 1, NULL, 0, NULL));

	/* Create an empty repo and unbundle into it */
	cl_git_pass(git_str_joinpath(&path,
		clar_sandbox_path(), "clone.git"));
	cl_git_pass(git_repository_init(&clone_repo, path.ptr, 1));

	cl_git_pass(git_bundle_open(&bundle, bundle_path.ptr));
	cl_git_pass(git_bundle_unbundle(clone_repo, bundle, NULL, NULL, NULL));

	/* Verify the ref exists and points to the same OID */
	cl_git_pass(git_reference_name_to_id(
		&unbundled_oid, clone_repo, "refs/heads/master"));
	cl_assert_equal_oid(&original_oid, &unbundled_oid);

	git_bundle_free(bundle);
	git_repository_free(clone_repo);
	git_str_dispose(&path);
	git_str_dispose(&bundle_path);
}

void test_bundle_bundle__unbundle_missing_prereq_fails(void)
{
	git_bundle *bundle = NULL;
	git_repository *empty_repo = NULL;
	git_str path = GIT_STR_INIT;

	cl_git_pass(git_bundle_open(&bundle,
		cl_fixture("bundle_incremental.v2")));

	/* Create an empty repo that won't have the prerequisites */
	cl_git_pass(git_str_joinpath(&path,
		clar_sandbox_path(), "empty_unbundle.git"));
	cl_git_pass(git_repository_init(&empty_repo, path.ptr, 1));

	cl_assert_equal_i(GIT_ENOTFOUND,
		git_bundle_unbundle(empty_repo, bundle, NULL, NULL, NULL));

	git_repository_free(empty_repo);
	cl_fixture_cleanup("empty_unbundle.git");
	git_str_dispose(&path);
	git_bundle_free(bundle);
}

/* --- Security fix tests --- */

/*
 * Fix #1: A bundle with more than GIT_BUNDLE_MAX_PREREQUISITES entries
 * must be rejected to prevent memory exhaustion.
 */
void test_bundle_bundle__too_many_prerequisites_rejected(void)
{
	git_bundle *bundle = NULL;
	git_str path = GIT_STR_INIT;
	git_str content = GIT_STR_INIT;
	int i;

	cl_git_pass(git_str_puts(&content, "# v2 git bundle\n"));
	for (i = 0; i <= GIT_BUNDLE_MAX_PREREQUISITES; i++) {
		cl_git_pass(git_str_puts(&content,
			"-0000000000000000000000000000000000000000\n"));
	}

	cl_git_pass(git_str_joinpath(&path,
		clar_sandbox_path(), "toomany_prereqs.bundle"));
	cl_git_pass(git_futils_writebuffer(
		&content, path.ptr, O_WRONLY | O_CREAT | O_TRUNC, 0644));

	cl_git_fail(git_bundle_open(&bundle, path.ptr));
	cl_assert(bundle == NULL);

	git_str_dispose(&path);
	git_str_dispose(&content);
}

/*
 * Fix #1: A bundle with more than GIT_BUNDLE_MAX_REFS entries must be
 * rejected to prevent memory exhaustion.
 */
void test_bundle_bundle__too_many_refs_rejected(void)
{
	git_bundle *bundle = NULL;
	git_str path = GIT_STR_INIT;
	git_str content = GIT_STR_INIT;
	int i;

	cl_git_pass(git_str_puts(&content, "# v2 git bundle\n"));
	for (i = 0; i <= GIT_BUNDLE_MAX_REFS; i++) {
		char line[80];
		p_snprintf(line, sizeof(line),
			"0000000000000000000000000000000000000000 "
			"refs/heads/branch%d\n", i);
		cl_git_pass(git_str_puts(&content, line));
	}

	cl_git_pass(git_str_joinpath(&path,
		clar_sandbox_path(), "toomany_refs.bundle"));
	cl_git_pass(git_futils_writebuffer(
		&content, path.ptr, O_WRONLY | O_CREAT | O_TRUNC, 0644));

	cl_git_fail(git_bundle_open(&bundle, path.ptr));
	cl_assert(bundle == NULL);

	git_str_dispose(&path);
	git_str_dispose(&content);
}

/*
 * Fix #6: A bundle containing an invalid refname must be rejected at
 * parse time, before any refname reaches ref-creation or filesystem code.
 */
void test_bundle_bundle__invalid_refname_rejected(void)
{
	git_bundle *bundle = NULL;
	git_str path = GIT_STR_INIT;
	git_str content = GIT_STR_INIT;

	/* refname with embedded space — invalid per git ref rules */
	cl_git_pass(git_str_puts(&content,
		"# v2 git bundle\n"
		"0000000000000000000000000000000000000000 "
		"refs/heads/bad name\n"
		"\n"));

	cl_git_pass(git_str_joinpath(&path,
		clar_sandbox_path(), "badrefname.bundle"));
	cl_git_pass(git_futils_writebuffer(
		&content, path.ptr, O_WRONLY | O_CREAT | O_TRUNC, 0644));

	cl_git_fail(git_bundle_open(&bundle, path.ptr));
	cl_assert(bundle == NULL);

	git_str_dispose(&path);
	git_str_dispose(&content);
}

/*
 * Fix #6: A bundle with a path-traversal-style refname must be rejected.
 */
void test_bundle_bundle__path_traversal_refname_rejected(void)
{
	git_bundle *bundle = NULL;
	git_str path = GIT_STR_INIT;
	git_str content = GIT_STR_INIT;

	cl_git_pass(git_str_puts(&content,
		"# v2 git bundle\n"
		"0000000000000000000000000000000000000000 "
		"../../../etc/evil\n"
		"\n"));

	cl_git_pass(git_str_joinpath(&path,
		clar_sandbox_path(), "traversal.bundle"));
	cl_git_pass(git_futils_writebuffer(
		&content, path.ptr, O_WRONLY | O_CREAT | O_TRUNC, 0644));

	cl_git_fail(git_bundle_open(&bundle, path.ptr));
	cl_assert(bundle == NULL);

	git_str_dispose(&path);
	git_str_dispose(&content);
}

/*
 * Fix #4: Unbundling into a repo where a ref already exists must fail
 * by default (force_update_refs = 0) without modifying the repository.
 */
void test_bundle_bundle__unbundle_no_force_fails_on_existing_ref(void)
{
	git_bundle *bundle = NULL;
	git_repository *clone_repo = NULL;
	git_str repo_path = GIT_STR_INIT;
	git_str bundle_path = GIT_STR_INIT;
	const char *refnames[] = { "refs/heads/master" };
	git_oid original_oid;

	/* Create a bundle */
	cl_git_pass(git_str_joinpath(&bundle_path,
		clar_sandbox_path(), "noforce.bundle"));
	cl_git_pass(git_bundle_create(
		bundle_path.ptr, _repo, refnames, 1, NULL, 0, NULL));

	/* Create a clone and do a first unbundle to populate refs */
	cl_git_pass(git_str_joinpath(&repo_path,
		clar_sandbox_path(), "clone_noforce.git"));
	cl_git_pass(git_repository_init(&clone_repo, repo_path.ptr, 1));

	cl_git_pass(git_bundle_open(&bundle, bundle_path.ptr));
	cl_git_pass(git_bundle_unbundle(
		clone_repo, bundle, NULL, NULL, NULL));
	git_bundle_free(bundle);
	bundle = NULL;

	/* Record the OID that was written */
	cl_git_pass(git_reference_name_to_id(
		&original_oid, clone_repo, "refs/heads/master"));

	/* A second unbundle without force must fail */
	cl_git_pass(git_bundle_open(&bundle, bundle_path.ptr));
	cl_assert_equal_i(GIT_EEXISTS,
		git_bundle_unbundle(clone_repo, bundle, NULL, NULL, NULL));

	/* The ref must be unchanged */
	{
		git_oid after_oid;
		cl_git_pass(git_reference_name_to_id(
			&after_oid, clone_repo, "refs/heads/master"));
		cl_assert_equal_oid(&original_oid, &after_oid);
	}

	git_bundle_free(bundle);
	git_repository_free(clone_repo);
	cl_fixture_cleanup("clone_noforce.git");
	git_str_dispose(&repo_path);
	git_str_dispose(&bundle_path);
}

/*
 * Fix #4: With force_update_refs set, unbundling over an existing ref
 * must succeed and update it.
 */
void test_bundle_bundle__unbundle_force_overwrites_existing_ref(void)
{
	git_bundle *bundle = NULL;
	git_repository *clone_repo = NULL;
	git_str repo_path = GIT_STR_INIT;
	git_str bundle_path = GIT_STR_INIT;
	const char *refnames[] = { "refs/heads/master" };
	git_oid expected_oid;
	git_bundle_unbundle_options force_opts =
		GIT_BUNDLE_UNBUNDLE_OPTIONS_INIT;

	force_opts.force_update_refs = 1;

	/* Create a bundle */
	cl_git_pass(git_str_joinpath(&bundle_path,
		clar_sandbox_path(), "force.bundle"));
	cl_git_pass(git_bundle_create(
		bundle_path.ptr, _repo, refnames, 1, NULL, 0, NULL));

	cl_git_pass(git_reference_name_to_id(
		&expected_oid, _repo, "refs/heads/master"));

	/* First unbundle into a fresh repo */
	cl_git_pass(git_str_joinpath(&repo_path,
		clar_sandbox_path(), "clone_force.git"));
	cl_git_pass(git_repository_init(&clone_repo, repo_path.ptr, 1));

	cl_git_pass(git_bundle_open(&bundle, bundle_path.ptr));
	cl_git_pass(git_bundle_unbundle(
		clone_repo, bundle, NULL, NULL, NULL));
	git_bundle_free(bundle);
	bundle = NULL;

	/* Second unbundle with force=1 must succeed */
	cl_git_pass(git_bundle_open(&bundle, bundle_path.ptr));
	cl_git_pass(git_bundle_unbundle(
		clone_repo, bundle, NULL, NULL, &force_opts));

	/* Ref must point to the expected OID */
	{
		git_oid result_oid;
		cl_git_pass(git_reference_name_to_id(
			&result_oid, clone_repo, "refs/heads/master"));
		cl_assert_equal_oid(&expected_oid, &result_oid);
	}

	git_bundle_free(bundle);
	git_repository_free(clone_repo);
	cl_fixture_cleanup("clone_force.git");
	git_str_dispose(&repo_path);
	git_str_dispose(&bundle_path);
}

/*
 * Fix #3: Verify that git_bundle_create produces a consistent bundle even
 * when called on a ref.  The header OID and the pack content must agree
 * (we test this by creating, parsing, and unbundling, then checking that
 * the resolved OID matches master in the source repo).
 */
void test_bundle_bundle__create_header_and_pack_oid_consistent(void)
{
	git_bundle *bundle = NULL;
	git_repository *clone_repo = NULL;
	git_str repo_path = GIT_STR_INIT;
	git_str bundle_path = GIT_STR_INIT;
	const char *refnames[] = { "refs/heads/master" };
	git_oid src_oid, bundled_oid, clone_oid;

	cl_git_pass(git_reference_name_to_id(
		&src_oid, _repo, "refs/heads/master"));

	cl_git_pass(git_str_joinpath(&bundle_path,
		clar_sandbox_path(), "consistent.bundle"));
	cl_git_pass(git_bundle_create(
		bundle_path.ptr, _repo, refnames, 1, NULL, 0, NULL));

	/* OID in the parsed bundle header must equal the source ref */
	cl_git_pass(git_bundle_open(&bundle, bundle_path.ptr));
	{
		const git_oid *hdr_oid;
		cl_git_pass(git_bundle_ref_byindex(
			&hdr_oid, NULL, bundle, 0));
		git_oid_cpy(&bundled_oid, hdr_oid);
	}
	cl_assert_equal_oid(&src_oid, &bundled_oid);

	/* OID written into the clone after unbundle must also match */
	cl_git_pass(git_str_joinpath(&repo_path,
		clar_sandbox_path(), "clone_consistent.git"));
	cl_git_pass(git_repository_init(&clone_repo, repo_path.ptr, 1));
	cl_git_pass(git_bundle_unbundle(
		clone_repo, bundle, NULL, NULL, NULL));
	cl_git_pass(git_reference_name_to_id(
		&clone_oid, clone_repo, "refs/heads/master"));
	cl_assert_equal_oid(&src_oid, &clone_oid);

	git_bundle_free(bundle);
	git_repository_free(clone_repo);
	cl_fixture_cleanup("clone_consistent.git");
	git_str_dispose(&repo_path);
	git_str_dispose(&bundle_path);
}

/* --- v3 bundle format tests (Fix B) --- */

/*
 * A v3 bundle with no capability lines (SHA-1 is implicit) must be
 * parsed exactly like a v2 bundle.
 */
void test_bundle_bundle__open_v3_no_capabilities(void)
{
	git_bundle *bundle = NULL;
	git_str path = GIT_STR_INIT;
	git_str content = GIT_STR_INIT;

	/* Minimal v3 bundle: signature + one ref + blank line (no pack) */
	cl_git_pass(git_str_puts(&content,
		"# v3 git bundle\n"
		"a65fedf39aefe402d3bb6e24df4d4f5fe4547750 refs/heads/master\n"
		"\n"));

	cl_git_pass(git_str_joinpath(&path,
		clar_sandbox_path(), "v3_nocat.bundle"));
	cl_git_pass(git_futils_writebuffer(
		&content, path.ptr, O_WRONLY | O_CREAT | O_TRUNC, 0644));

	cl_git_pass(git_bundle_open(&bundle, path.ptr));
	cl_assert_equal_sz(1, git_bundle_ref_count(bundle));

	git_bundle_free(bundle);
	git_str_dispose(&path);
	git_str_dispose(&content);
}

/*
 * A v3 bundle with an explicit @object-format=sha1 capability must be
 * accepted (SHA-1 is our native format).
 */
void test_bundle_bundle__open_v3_explicit_sha1(void)
{
	git_bundle *bundle = NULL;
	git_str path = GIT_STR_INIT;
	git_str content = GIT_STR_INIT;

	cl_git_pass(git_str_puts(&content,
		"# v3 git bundle\n"
		"@object-format=sha1\n"
		"a65fedf39aefe402d3bb6e24df4d4f5fe4547750 refs/heads/master\n"
		"\n"));

	cl_git_pass(git_str_joinpath(&path,
		clar_sandbox_path(), "v3_sha1.bundle"));
	cl_git_pass(git_futils_writebuffer(
		&content, path.ptr, O_WRONLY | O_CREAT | O_TRUNC, 0644));

	cl_git_pass(git_bundle_open(&bundle, path.ptr));
	cl_assert_equal_sz(1, git_bundle_ref_count(bundle));

	git_bundle_free(bundle);
	git_str_dispose(&path);
	git_str_dispose(&content);
}

/*
 * A v3 bundle with @object-format=sha256 must be rejected with
 * GIT_ENOTSUPPORTED — not a generic parse failure.
 */
void test_bundle_bundle__open_v3_sha256_rejected(void)
{
	git_bundle *bundle = NULL;
	git_str path = GIT_STR_INIT;
	git_str content = GIT_STR_INIT;

	cl_git_pass(git_str_puts(&content,
		"# v3 git bundle\n"
		"@object-format=sha256\n"
		"0000000000000000000000000000000000000000000000000000000000000001"
		" refs/heads/master\n"
		"\n"));

	cl_git_pass(git_str_joinpath(&path,
		clar_sandbox_path(), "v3_sha256.bundle"));
	cl_git_pass(git_futils_writebuffer(
		&content, path.ptr, O_WRONLY | O_CREAT | O_TRUNC, 0644));

	cl_assert_equal_i(GIT_ENOTSUPPORTED,
		git_bundle_open(&bundle, path.ptr));
	cl_assert(bundle == NULL);

	git_str_dispose(&path);
	git_str_dispose(&content);
}

/*
 * Unknown v3 capabilities must be silently ignored (forward compatibility).
 */
void test_bundle_bundle__open_v3_unknown_capability_ignored(void)
{
	git_bundle *bundle = NULL;
	git_str path = GIT_STR_INIT;
	git_str content = GIT_STR_INIT;

	cl_git_pass(git_str_puts(&content,
		"# v3 git bundle\n"
		"@object-format=sha1\n"
		"@some-future-capability=value\n"
		"@another-cap\n"
		"a65fedf39aefe402d3bb6e24df4d4f5fe4547750 refs/heads/master\n"
		"\n"));

	cl_git_pass(git_str_joinpath(&path,
		clar_sandbox_path(), "v3_unknowncap.bundle"));
	cl_git_pass(git_futils_writebuffer(
		&content, path.ptr, O_WRONLY | O_CREAT | O_TRUNC, 0644));

	cl_git_pass(git_bundle_open(&bundle, path.ptr));
	cl_assert_equal_sz(1, git_bundle_ref_count(bundle));

	git_bundle_free(bundle);
	git_str_dispose(&path);
	git_str_dispose(&content);
}

/* --- Annotated tag test (Fix A) --- */

/*
 * When a bundle includes a ref pointing to an annotated tag, the tag
 * object itself must appear in the pack (not just the peeled commit).
 * Previously git_packbuilder_insert_walk would silently omit it.
 */
void test_bundle_bundle__create_preserves_annotated_tag(void)
{
	git_repository *src = NULL, *dst = NULL;
	git_signature *sig = NULL;
	git_oid blob_oid, tree_oid, commit_oid, tag_oid;
	git_treebuilder *tb = NULL;
	git_tree *tree = NULL;
	git_object *commit_obj = NULL;
	git_bundle *bundle = NULL;
	git_odb *odb = NULL;
	git_str src_path = GIT_STR_INIT;
	git_str dst_path = GIT_STR_INIT;
	git_str bundle_path = GIT_STR_INIT;
	const char *refnames[] = { "refs/tags/annotated" };
	git_oid ref_oid;

	cl_git_pass(git_str_joinpath(&src_path,
		clar_sandbox_path(), "anntag_src.git"));
	cl_git_pass(git_str_joinpath(&dst_path,
		clar_sandbox_path(), "anntag_dst.git"));
	cl_git_pass(git_str_joinpath(&bundle_path,
		clar_sandbox_path(), "annotated.bundle"));

	/* Build a minimal source repo with one commit */
	cl_git_pass(git_repository_init(&src, src_path.ptr, 1));
	cl_git_pass(git_signature_now(&sig, "Test User", "test@example.com"));

	cl_git_pass(git_blob_create_from_buffer(
		&blob_oid, src, "content", 7));
	cl_git_pass(git_treebuilder_new(&tb, src, NULL));
	cl_git_pass(git_treebuilder_insert(
		NULL, tb, "file.txt", &blob_oid, GIT_FILEMODE_BLOB));
	cl_git_pass(git_treebuilder_write(&tree_oid, tb));
	git_treebuilder_free(tb);

	cl_git_pass(git_tree_lookup(&tree, src, &tree_oid));
	cl_git_pass(git_commit_create(
		&commit_oid, src, "refs/heads/master",
		sig, sig, NULL, "initial", tree, 0, NULL));
	git_tree_free(tree);

	/* Create an annotated tag pointing to that commit */
	cl_git_pass(git_object_lookup(
		&commit_obj, src, &commit_oid, GIT_OBJECT_COMMIT));
	cl_git_pass(git_tag_create(
		&tag_oid, src, "annotated",
		commit_obj, sig, "release v1.0", 0));
	git_object_free(commit_obj);

	/* Bundle using the tag ref */
	cl_git_pass(git_bundle_create(
		bundle_path.ptr, src, refnames, 1, NULL, 0, NULL));

	/* Unbundle into a fresh repo */
	cl_git_pass(git_repository_init(&dst, dst_path.ptr, 1));
	cl_git_pass(git_bundle_open(&bundle, bundle_path.ptr));
	cl_git_pass(git_bundle_unbundle(dst, bundle, NULL, NULL, NULL));

	/* The tag OBJECT must be in the destination ODB */
	cl_git_pass(git_repository_odb(&odb, dst));
	cl_assert_equal_b(true, git_odb_exists(odb, &tag_oid));
	git_odb_free(odb);

	/* The ref must point to the tag object, not the peeled commit */
	cl_git_pass(git_reference_name_to_id(
		&ref_oid, dst, "refs/tags/annotated"));
	cl_assert_equal_oid(&tag_oid, &ref_oid);

	git_bundle_free(bundle);
	git_signature_free(sig);
	git_repository_free(src);
	git_repository_free(dst);
	cl_fixture_cleanup("anntag_src.git");
	cl_fixture_cleanup("anntag_dst.git");
	git_str_dispose(&src_path);
	git_str_dispose(&dst_path);
	git_str_dispose(&bundle_path);
}
