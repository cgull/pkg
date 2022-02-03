/*-
 * Copyright (c) 2011-2019 Baptiste Daroussin <bapt@FreeBSD.org>
 * Copyright (c) 2011-2012 Julien Laffaye <jlaffaye@FreeBSD.org>
 * Copyright (c) 2011-2012 Marin Atanasov Nikolov <dnaeon@gmail.com>
 * Copyright (c) 2012-2015 Matthew Seaman <matthew@FreeBSD.org>
 * Copyright (c) 2014 Vsevolod Stakhov <vsevolod@FreeBSD.org>
 *
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer
 *    in this position and unchanged.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR(S) ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL THE AUTHOR(S) BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 * NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/uio.h>

#include <archive_entry.h>
#include <assert.h>
#include <fts.h>
#include <libgen.h>
#include <sqlite3.h>
#include <string.h>
#include <dirent.h>
#define _WITH_GETLINE
#include <stdio.h>
#include <stdbool.h>
#include <unistd.h>
#include <errno.h>
#include <sys/mman.h>
#include <fcntl.h>

#include "pkg.h"
#include "private/event.h"
#include "private/utils.h"
#include "private/pkg.h"
#include "private/pkgdb.h"

struct sig_cert {
	char name[MAXPATHLEN];
	char *sig;
	int64_t siglen;
	char *cert;
	int64_t certlen;
	bool cert_allocated;
	bool trusted;
};

static int
pkg_repo_fetch_remote_tmp(struct pkg_repo *repo,
  const char *filename, const char *extension, time_t *t, int *rc, bool silent)
{
	char url[MAXPATHLEN];
	char tmp[MAXPATHLEN];
	int fd;
	const char *tmpdir, *dot;

	/*
	 * XXX: here we support old naming scheme, such as filename.yaml
	 */
	dot = strrchr(filename, '.');
	if (dot != NULL) {
		snprintf(tmp, MIN(sizeof(tmp), dot - filename + 1), "%s", filename);
		snprintf(url, sizeof(url), "%s/%s.%s", pkg_repo_url(repo), tmp,
				extension);
	}
	else {
		snprintf(url, sizeof(url), "%s/%s.%s", pkg_repo_url(repo), filename,
				extension);
	}

	tmpdir = getenv("TMPDIR");
	if (tmpdir == NULL)
		tmpdir = "/tmp";
	mkdirs(tmpdir);
	snprintf(tmp, sizeof(tmp), "%s/%s.%s.XXXXXX", tmpdir, filename, extension);

	fd = mkstemp(tmp);
	if (fd == -1) {
		pkg_emit_error("Could not create temporary file %s, "
		    "aborting update.\n", tmp);
		*rc = EPKG_FATAL;
		return (-1);
	}
	(void)unlink(tmp);

	if ((*rc = pkg_fetch_file_to_fd(repo, url, fd, t, -1, 0, silent)) != EPKG_OK) {
		close(fd);
		fd = -1;
	}

	return (fd);
}

static bool
pkg_repo_file_has_ext(const char *path, const char *ext)
{
	size_t n, l;
	const char *p = NULL;

	n = strlen(path);
	l = strlen(ext);
	p = &path[n - l];

	if (strcmp(p, ext) == 0)
		return (true);

	return (false);
}

static bool
pkg_repo_check_fingerprint(struct pkg_repo *repo, pkghash *sc, bool fatal)
{
	char *hash;
	int nbgood = 0;
	struct sig_cert *s = NULL;
	struct pkg_repo_meta_key *mk = NULL;
	pkghash_it it;

	if (pkghash_count(sc) == 0) {
		if (fatal)
			pkg_emit_error("No signature found");
		return (false);
	}

	/* load fingerprints */
	if (repo->trusted_fp == NULL) {
		if (pkg_repo_load_fingerprints(repo) != EPKG_OK)
			return (false);
	}

	it = pkghash_iterator(sc);
	while (pkghash_next(&it)) {
		s = (struct sig_cert *) it.value;
		if (s->sig != NULL && s->cert == NULL) {
			/*
			 * We may want to check meta
			 */
			if (repo->meta != NULL && repo->meta->keys != NULL) {
				mk = pkghash_get_value(repo->meta->keys, s->name);
			}

			if (mk != NULL && mk->pubkey != NULL) {
				s->cert = mk->pubkey;
				s->certlen = strlen(mk->pubkey);
			}
			else {
				if (fatal)
					pkg_emit_error("No key with name %s has been found", s->name);
				return (false);
			}
		}
		else if (s->sig == NULL) {
			if (fatal)
				pkg_emit_error("No signature with name %s has been found", s->name);
			return (false);
		}

		s->trusted = false;
		hash = pkg_checksum_data(s->cert, s->certlen,
		    PKG_HASH_TYPE_SHA256_HEX);
		if (pkghash_get(repo->revoked_fp, hash) != NULL) {
			if (fatal)
				pkg_emit_error("At least one of the "
					"certificates has been revoked");

			free(hash);
			return (false);
		}

		if (pkghash_get(repo->trusted_fp, hash) != NULL) {
			nbgood++;
			s->trusted = true;
		}
		free(hash);
	}

	if (nbgood == 0) {
		if (fatal)
			pkg_emit_error("No trusted public keys found");

		return (false);
	}

	return (true);
}

static void
pkg_repo_signatures_free(pkghash *sc)
{
	struct sig_cert *s;
	pkghash_it it;

	if (sc == NULL)
		return;
	it = pkghash_iterator(sc);
	while (pkghash_next(&it)) {
		s = (struct sig_cert *)it.value;
		free(s->sig);
		if (s->cert_allocated)
			free(s->cert);
		free(s);
	}
	pkghash_destroy(sc);
}


struct pkg_extract_cbdata {
	int afd;
	int tfd;
	const char *fname;
	bool need_sig;
};

static int
pkg_repo_write_sig_from_archive(struct archive *a, int fd, size_t siglen)
{
	char sig[siglen];

	if (archive_read_data(a, sig, siglen) == -1) {
		pkg_emit_errno("pkg_repo_meta_extract_signature",
		    "archive_read_data failed");
		return (EPKG_FATAL);
	}
	if (write(fd, sig, siglen) == -1) {
		pkg_emit_errno("pkg_repo_meta_extract_signature",
		    "write failed");
		return (EPKG_FATAL);
	}
	return (EPKG_OK);
}

static int
pkg_repo_meta_extract_signature_pubkey(int fd, void *ud)
{
	struct archive *a = NULL;
	struct archive_entry *ae = NULL;
	struct pkg_extract_cbdata *cb = ud;
	int siglen;
	int rc = EPKG_FATAL;

	pkg_debug(1, "PkgRepo: extracting signature of repo in a sandbox");

	a = archive_read_new();
	archive_read_support_filter_all(a);
	archive_read_support_format_tar(a);

	archive_read_open_fd(a, cb->afd, 4096);

	while (archive_read_next_header(a, &ae) == ARCHIVE_OK) {
		if (cb->need_sig && strcmp(archive_entry_pathname(ae), "signature") == 0) {
			siglen = archive_entry_size(ae);
			rc = pkg_repo_write_sig_from_archive(a, fd, siglen);
			if (rc != EPKG_OK)
				break;
		}
		else if (strcmp(archive_entry_pathname(ae), cb->fname) == 0) {
			if (archive_read_data_into_fd(a, cb->tfd) != 0) {
				pkg_emit_error("Error extracting the archive: '%s'", archive_error_string(a));
				rc = EPKG_FATAL;
				break;
			}
			else if (!cb->need_sig) {
				rc = EPKG_OK;
			}
		}
	}

	close(cb->tfd);
	/*
	 * XXX: do not free resources here since the sandbox is terminated anyway
	 */
	return (rc);
}
/*
 * We use here the following format:
 * <type(0|1)><namelen(int)><name><datalen(int)><data>
 */
static int
pkg_repo_meta_extract_signature_fingerprints(int fd, void *ud)
{
	struct archive *a = NULL;
	struct archive_entry *ae = NULL;
	struct pkg_extract_cbdata *cb = ud;
	int siglen, keylen;
	void *sig;
	int rc = EPKG_FATAL;
	char key[MAXPATHLEN], t;
	struct iovec iov[5];

	pkg_debug(1, "PkgRepo: extracting signature of repo in a sandbox");

	a = archive_read_new();
	archive_read_support_filter_all(a);
	archive_read_support_format_tar(a);

	archive_read_open_fd(a, cb->afd, 4096);

	while (archive_read_next_header(a, &ae) == ARCHIVE_OK) {
		if (pkg_repo_file_has_ext(archive_entry_pathname(ae), ".sig")) {
			snprintf(key, sizeof(key), "%.*s",
					(int) strlen(archive_entry_pathname(ae)) - 4,
					archive_entry_pathname(ae));
			siglen = archive_entry_size(ae);
			sig = xmalloc(siglen);
			if (archive_read_data(a, sig, siglen) == -1) {
				pkg_emit_errno("pkg_repo_meta_extract_signature",
						"archive_read_data failed");
				free(sig);
				return (EPKG_FATAL);
			}
			/* Signature type */
			t = 0;
			keylen = strlen(key);
			iov[0].iov_base = &t;
			iov[0].iov_len = sizeof(t);
			iov[1].iov_base = &keylen;
			iov[1].iov_len = sizeof(keylen);
			iov[2].iov_base = key;
			iov[2].iov_len = keylen;
			iov[3].iov_base = &siglen;
			iov[3].iov_len = sizeof(siglen);
			iov[4].iov_base = sig;
			iov[4].iov_len = siglen;
			if (writev(fd, iov, NELEM(iov)) == -1) {
				pkg_emit_errno("pkg_repo_meta_extract_signature",
						"writev failed");
				free(sig);
				return (EPKG_FATAL);
			}
			free(sig);
			rc = EPKG_OK;
		}
		else if (pkg_repo_file_has_ext(archive_entry_pathname(ae), ".pub")) {
			snprintf(key, sizeof(key), "%.*s",
					(int) strlen(archive_entry_pathname(ae)) - 4,
					archive_entry_pathname(ae));
			siglen = archive_entry_size(ae);
			sig = xmalloc(siglen);
			if (archive_read_data(a, sig, siglen) == -1) {
				pkg_emit_errno("pkg_repo_meta_extract_signature",
						"archive_read_data failed");
				free(sig);
				return (EPKG_FATAL);
			}
			/* Pubkey type */
			t = 1;
			keylen = strlen(key);
			iov[0].iov_base = &t;
			iov[0].iov_len = sizeof(t);
			iov[1].iov_base = &keylen;
			iov[1].iov_len = sizeof(keylen);
			iov[2].iov_base = key;
			iov[2].iov_len = keylen;
			iov[3].iov_base = &siglen;
			iov[3].iov_len = sizeof(siglen);
			iov[4].iov_base = sig;
			iov[4].iov_len = siglen;
			if (writev(fd, iov, NELEM(iov)) == -1) {
				pkg_emit_errno("pkg_repo_meta_extract_signature",
						"writev failed");
				free(sig);
				return (EPKG_FATAL);
			}
			free(sig);
			rc = EPKG_OK;
		}
		else {
			if (strcmp(archive_entry_pathname(ae), cb->fname) == 0) {
				if (archive_read_data_into_fd(a, cb->tfd) != 0) {
					pkg_emit_error("Error extracting the archive: '%s'", archive_error_string(a));
					rc = EPKG_FATAL;
					break;
				}
			}
		}
	}
	close(cb->tfd);
	/*
	 * XXX: do not free resources here since the sandbox is terminated anyway
	 */
	return (rc);
}

static int
pkg_repo_parse_sigkeys(const char *in, int inlen, pkghash **sc)
{
	const char *p = in, *end = in + inlen;
	int rc = EPKG_OK;
	enum {
		fp_parse_type,
		fp_parse_flen,
		fp_parse_file,
		fp_parse_siglen,
		fp_parse_sig
	} state = fp_parse_type;
	char type = 0;
	unsigned char *sig;
	int len = 0, tlen;
	struct sig_cert *s = NULL;
	bool new = false;

	while (p < end) {
		switch (state) {
		case fp_parse_type:
			type = *p;
			if (type != 0 && type != 1) {
				/* Invalid type */
				pkg_emit_error("%d is not a valid type for signature_fingerprints"
						" output", type);
				return (EPKG_FATAL);
			}
			state = fp_parse_flen;
			s = NULL;
			p ++;
			break;
		case fp_parse_flen:
			if (end - p < sizeof (int)) {
				pkg_emit_error("truncated reply for signature_fingerprints"
						" output");
				return (EPKG_FATAL);
			}
			memcpy(&len, p, sizeof(int));
			state = fp_parse_file;
			p += sizeof(int);
			s = NULL;
			break;
		case fp_parse_file:
			if (end - p < len || len <= 0) {
				pkg_emit_error("truncated reply for signature_fingerprints"
						" output, wanted %d bytes", len);
				return (EPKG_FATAL);
			}
			else if (len >= MAXPATHLEN) {
				pkg_emit_error("filename is incorrect for signature_fingerprints"
						" output: %d, wanted 5..%d bytes", type, len);
				free(s);
				return (EPKG_FATAL);
			}
			char *k = xstrndup(p, len);
			s = pkghash_get_value(*sc, k);
			free(k);
			if ( s == NULL) {
				s = xcalloc(1, sizeof(struct sig_cert));
				tlen = MIN(len, sizeof(s->name) - 1);
				memcpy(s->name, p, tlen);
				s->name[tlen] = '\0';
				new = true;
			} else {
				new = false;
			}
			state = fp_parse_siglen;
			p += len;
			break;
		case fp_parse_siglen:
			if (s == NULL) {
				pkg_emit_error("fatal state machine failure at pkg_repo_parse_sigkeys");
				return (EPKG_FATAL);
			}
			if (end - p < sizeof (int)) {
				pkg_emit_error("truncated reply for signature_fingerprints"
						"output");
				free(s);
				return (EPKG_FATAL);
			}
			memcpy(&len, p, sizeof(int));
			state = fp_parse_sig;
			p += sizeof(int);
			break;
		case fp_parse_sig:
			if (s == NULL) {
				pkg_emit_error("fatal state machine failure at pkg_repo_parse_sigkeys");
				return (EPKG_FATAL);
			}
			if (end - p < len || len <= 0) {
				pkg_emit_error("truncated reply for signature_fingerprints"
						"output, wanted %d bytes", len);
				free(s);
				return (EPKG_FATAL);
			}
			sig = xmalloc(len);
			memcpy(sig, p, len);
			if (type == 0) {
				s->sig = sig;
				s->siglen = len;
			}
			else {
				s->cert = sig;
				s->certlen = len;
				s->cert_allocated = true;
			}
			state = fp_parse_type;
			p += len;

			if (new)
				pkghash_safe_add(*sc, s->name, s, NULL);

			break;
		}
	}

	return (rc);
}

static int
pkg_repo_archive_extract_archive(int fd, const char *file,
    struct pkg_repo *repo, int dest_fd,
    pkghash **signatures)
{
	struct pkghash *sc = NULL;
	struct sig_cert *s;
	struct pkg_extract_cbdata cbdata;

	char *sig = NULL;
	int rc = EPKG_OK;
	int64_t siglen = 0;


	pkg_debug(1, "PkgRepo: extracting %s of repo %s", file, pkg_repo_name(repo));

	/* Seek to the begin of file */
	(void)lseek(fd, 0, SEEK_SET);

	cbdata.afd = fd;
	cbdata.fname = file;
	cbdata.tfd = dest_fd;

	if (pkg_repo_signature_type(repo) == SIG_PUBKEY) {
		cbdata.need_sig = true;
		if (pkg_emit_sandbox_get_string(pkg_repo_meta_extract_signature_pubkey,
				&cbdata, (char **)&sig, &siglen) == EPKG_OK && sig != NULL) {
			s = xcalloc(1, sizeof(struct sig_cert));
			s->sig = sig;
			s->siglen = siglen;
			strlcpy(s->name, "signature", sizeof(s->name));
			pkghash_safe_add(sc, s->name, s, NULL);
		}
	}
	else if (pkg_repo_signature_type(repo) == SIG_FINGERPRINT) {
		if (pkg_emit_sandbox_get_string(pkg_repo_meta_extract_signature_fingerprints,
				&cbdata, (char **)&sig, &siglen) == EPKG_OK && sig != NULL &&
				siglen > 0) {
			if (pkg_repo_parse_sigkeys(sig, siglen, &sc) == EPKG_FATAL) {
				return (EPKG_FATAL);
			}
			free(sig);
			if (!pkg_repo_check_fingerprint(repo, sc, true)) {
				return (EPKG_FATAL);
			}
		}
		else {
			pkg_emit_error("No signature found");
			return (EPKG_FATAL);
		}
	}
	else {
		cbdata.need_sig = false;
		if (pkg_emit_sandbox_get_string(pkg_repo_meta_extract_signature_pubkey,
			&cbdata, (char **)&sig, &siglen) == EPKG_OK) {
			free(sig);
		}
		else {
			pkg_emit_error("Repo extraction failed");
			return (EPKG_FATAL);
		}
	}
	(void)lseek(fd, 0, SEEK_SET);
	if (dest_fd != -1)
		(void)lseek(dest_fd, 0, SEEK_SET);

	if (rc == EPKG_OK) {
		if (signatures != NULL)
			*signatures = sc;
		else
			pkg_repo_signatures_free(sc);
	}
	else {
		pkg_repo_signatures_free(sc);
	}

	return rc;
}

static int
pkg_repo_archive_extract_check_archive(int fd, const char *file,
    struct pkg_repo *repo, int dest_fd)
{
	pkghash *sc = NULL;
	struct sig_cert *s;
	pkghash_it it;
	int ret, rc;

	ret = rc = EPKG_OK;

	if (pkg_repo_archive_extract_archive(fd, file, repo, dest_fd, &sc)
			!= EPKG_OK)
		return (EPKG_FATAL);

	if (pkg_repo_signature_type(repo) == SIG_PUBKEY) {
		if (pkg_repo_key(repo) == NULL) {
			pkg_emit_error("No PUBKEY defined. Removing "
			    "repository.");
			rc = EPKG_FATAL;
			goto out;
		}
		if (sc == NULL) {
			pkg_emit_error("No signature found in the repository.  "
					"Can not validate against %s key.", pkg_repo_key(repo));
			rc = EPKG_FATAL;
			goto out;
		}
		it = pkghash_iterator(sc);
		pkghash_next(&it); /* check that there is content is already above */
		s = (struct sig_cert *)it.value;
		/*
		 * Here are dragons:
		 * 1) rsa_verify is NOT rsa_verify_cert
		 * 2) siglen must be reduced by one to match how pkg_repo_finish()
		 *    packs the signature in.
		 *
		 * by @bdrewery
		 */
		ret = rsa_verify(pkg_repo_key(repo), s->sig, s->siglen - 1,
		    dest_fd);
		if (ret != EPKG_OK) {
			pkg_emit_error("Invalid signature, "
					"removing repository.");
			rc = EPKG_FATAL;
			goto out;
		}
	}
	else if (pkg_repo_signature_type(repo) == SIG_FINGERPRINT) {
		it = pkghash_iterator(sc);
		while (pkghash_next(&it)) {
			s = (struct sig_cert *)it.value;
			ret = rsa_verify_cert(s->cert, s->certlen, s->sig, s->siglen,
				dest_fd);
			if (ret == EPKG_OK && s->trusted) {
				break;
			}
			ret = EPKG_FATAL;
		}
		if (ret != EPKG_OK) {
			pkg_emit_error("No trusted certificate has been used "
			    "to sign the repository");
			rc = EPKG_FATAL;
			goto out;
		}
	}

out:
	return rc;
}

int
pkg_repo_fetch_remote_extract_fd(struct pkg_repo *repo, const char *filename,
    time_t *t, int *rc, size_t *sz)
{
	int fd, dest_fd;
	const char *tmpdir;
	char tmp[MAXPATHLEN];
	struct stat st;

	fd = pkg_repo_fetch_remote_tmp(repo, filename, "pkg", t, rc, false);
	if (fd == -1) {
		fd = pkg_repo_fetch_remote_tmp(repo, filename,
		    packing_format_to_string(repo->meta->packing_format), t, rc, false);
	}
	if (fd == -1)
		return (-1);

	tmpdir = getenv("TMPDIR");
	if (tmpdir == NULL)
		tmpdir = "/tmp";
	snprintf(tmp, sizeof(tmp), "%s/%s.XXXXXX", tmpdir, filename);

	dest_fd = mkstemp(tmp);
	if (dest_fd == -1) {
		pkg_emit_error("Could not create temporary file %s, "
				"aborting update.\n", tmp);
		close(fd);
		*rc = EPKG_FATAL;
		return (-1);
	}

	(void)unlink(tmp);
	if (pkg_repo_archive_extract_check_archive(fd, filename, repo, dest_fd)
			!= EPKG_OK) {
		*rc = EPKG_FATAL;
		close(dest_fd);
		close(fd);
		return (-1);
	}

	/* Thus removing archived file as well */
	close(fd);
	if (fstat(dest_fd, &st) == -1) {
		close(dest_fd);
		return (-1);
	}

	*sz = st.st_size;

	return (dest_fd);
}

struct pkg_repo_check_cbdata {
	unsigned char *map;
	size_t len;
	const char *name;
};

static int
pkg_repo_meta_extract_pubkey(int fd, void *ud)
{
	struct pkg_repo_check_cbdata *cbdata = ud;
	struct ucl_parser *parser;
	ucl_object_t *top;
	const ucl_object_t *obj, *cur, *elt;
	ucl_object_iter_t iter = NULL;
	struct iovec iov[2];
	int rc = EPKG_OK;
	int64_t res_len = 0;

	parser = ucl_parser_new(UCL_PARSER_NO_FILEVARS);
	if (!ucl_parser_add_chunk(parser, cbdata->map, cbdata->len)) {
		pkg_emit_error("cannot parse repository meta from %s",
				ucl_parser_get_error(parser));
		ucl_parser_free(parser);
		return (EPKG_FATAL);
	}

	top = ucl_parser_get_object(parser);
	ucl_parser_free(parser);

	/* Now search for the required key */
	obj = ucl_object_find_key(top, "cert");
	if (obj == NULL) {
		pkg_emit_error("cannot find key for signature %s in meta",
				cbdata->name);
		ucl_object_unref(top);
		return (EPKG_FATAL);
	}
	while((cur = ucl_iterate_object(obj, &iter, false)) != NULL) {
		elt = ucl_object_find_key(cur, "name");
		if (elt == NULL || elt->type != UCL_STRING)
			continue;
		if (strcmp(ucl_object_tostring(elt), cbdata->name) != 0)
			continue;
		elt = ucl_object_find_key(cur, "data");
		if (elt == NULL || elt->type != UCL_STRING)
			continue;

		/* +1 to include \0 at the end */
		res_len = elt->len + 1;
		iov[0].iov_base = (void *)ucl_object_tostring(elt);
		iov[0].iov_len = res_len;
		if (writev(fd, iov, 1) == -1) {
			pkg_emit_errno("pkg_repo_meta_extract_pubkey",
					"writev error");
			rc = EPKG_FATAL;
			break;
		}
	}

	ucl_object_unref(top);

	return (rc);
}

int
pkg_repo_fetch_meta(struct pkg_repo *repo, time_t *t)
{
	char filepath[MAXPATHLEN];
	struct pkg_repo_meta *nmeta;
	struct stat st;
	unsigned char *map = NULL;
	int fd, dbdirfd, metafd;
	int rc = EPKG_OK, ret;
	pkghash *sc = NULL;
	struct sig_cert *s;
	struct pkg_repo_check_cbdata cbdata;
	bool newscheme = false;
	pkghash_it it;

	dbdirfd = pkg_get_dbdirfd();
	snprintf(filepath, sizeof(filepath), "%s.meta", pkg_repo_name(repo));
	fd = pkg_repo_fetch_remote_tmp(repo, "meta", "conf", t, &rc, true);
	if (fd != -1) {
		newscheme = true;
		metafd = fd;
		fd = openat(dbdirfd, filepath, O_RDWR|O_CREAT|O_TRUNC, 0644);
		if (fd == -1) {
			close(metafd);
			return (EPKG_FATAL);
		}
		goto load_meta;
	} else if (rc == EPKG_UPTODATE) {
		return (EPKG_UPTODATE);
	}

	/* TODO: remove this backward compatibility some day */
	fd = pkg_repo_fetch_remote_tmp(repo, "meta", "txz", t, &rc, false);
	if (fd == -1)
		return (rc);

	metafd = openat(dbdirfd, filepath, O_RDWR|O_CREAT|O_TRUNC, 0644);
	if (metafd == -1) {
		close(fd);
		return (EPKG_FATAL);
	}

	if (pkg_repo_signature_type(repo) == SIG_PUBKEY) {
		if ((rc = pkg_repo_archive_extract_check_archive(fd, "meta", repo, metafd)) != EPKG_OK) {
			close (fd);
			return (rc);
		}
		goto load_meta;
	}

	/*
	 * For fingerprints we cannot just load pubkeys as they could be in metafile itself
	 * To do it, we parse meta in sandbox and for each unloaded pubkey we try to return
	 * a corresponding key from meta file.
	 */

	if ((rc = pkg_repo_archive_extract_archive(fd, "meta", repo,
	    metafd, &sc)) != EPKG_OK) {
		close(metafd);
		unlinkat(dbdirfd, filepath, 0);
		close (fd);
		return (rc);
	}
	close(metafd);
	close(fd);

	if (repo->signature_type == SIG_FINGERPRINT && repo->trusted_fp == NULL) {
		if (pkg_repo_load_fingerprints(repo) != EPKG_OK)
			return (EPKG_FATAL);
	}

	/* Map meta file for extracting pubkeys from it */
	if ((metafd = openat(dbdirfd, filepath, O_RDONLY)) == -1) {
		pkg_emit_errno("pkg_repo_fetch_meta", "cannot open meta fetched");
		rc = EPKG_FATAL;
		goto cleanup;
	}

	if (fstat(metafd, &st) == -1) {
		pkg_emit_errno("pkg_repo_fetch_meta", "cannot stat meta fetched");
		rc = EPKG_FATAL;
		goto cleanup;
	}

	map = mmap(NULL, st.st_size, PROT_READ, MAP_SHARED, fd, 0);
	if (map == MAP_FAILED) {
		pkg_emit_errno("pkg_repo_fetch_meta", "cannot mmap meta fetched");
		rc = EPKG_FATAL;
		goto cleanup;
	}

	if (repo->signature_type == SIG_FINGERPRINT) {
		cbdata.len = st.st_size;
		cbdata.map = map;
		it = pkghash_iterator(sc);
		while (pkghash_next(&it)) {
			s = (struct sig_cert *) it.value;
			if (s->siglen != 0 && s->certlen == 0) {
				/*
				 * We need to load this pubkey from meta
				 */
				cbdata.name = s->name;
				if (pkg_emit_sandbox_get_string(pkg_repo_meta_extract_pubkey, &cbdata,
						(char **)&s->cert, &s->certlen) != EPKG_OK) {
					rc = EPKG_FATAL;
					goto cleanup;
				}
				s->cert_allocated = true;
			}
		}

		if (!pkg_repo_check_fingerprint(repo, sc, true)) {
			rc = EPKG_FATAL;
			goto cleanup;
		}

		ret = EPKG_FATAL;
		it = pkghash_iterator(sc);
		while (pkghash_next(&it)) {
			s = (struct sig_cert *) it.value;
			ret = rsa_verify_cert(s->cert, s->certlen, s->sig, s->siglen,
				metafd);
			if (ret == EPKG_OK && s->trusted)
				break;
			ret = EPKG_FATAL;
		}
		if (ret != EPKG_OK) {
			pkg_emit_error("No trusted certificate has been used "
				"to sign the repository");
			rc = EPKG_FATAL;
			goto cleanup;
		}
	}

load_meta:
	if ((rc = pkg_repo_meta_load(metafd, &nmeta)) != EPKG_OK) {
		if (map != NULL)
			munmap(map, st.st_size);

		return (rc);
	} else if (newscheme) {
		pkg_repo_meta_dump_fd(nmeta, fd);
	}

	if (repo->meta != NULL)
		pkg_repo_meta_free(repo->meta);

	repo->meta = nmeta;

cleanup:
	if (map != NULL)
		munmap(map, st.st_size);

	if (sc != NULL)
		pkg_repo_signatures_free(sc);

	if (rc != EPKG_OK)
		unlinkat(dbdirfd, filepath, 0);

	return (rc);
}

static struct fingerprint *
pkg_repo_parse_fingerprint(ucl_object_t *obj)
{
	const ucl_object_t *cur;
	ucl_object_iter_t it = NULL;
	const char *function = NULL, *fp = NULL;
	hash_t fct = HASH_UNKNOWN;
	struct fingerprint *f = NULL;
	const char *key;

	while ((cur = ucl_iterate_object(obj, &it, true))) {
		key = ucl_object_key(cur);
		if (cur->type != UCL_STRING)
			continue;

		if (strcasecmp(key, "function") == 0) {
			function = ucl_object_tostring(cur);
			continue;
		}

		if (strcasecmp(key, "fingerprint") == 0) {
			fp = ucl_object_tostring(cur);
			continue;
		}
	}

	if (fp == NULL || function == NULL)
		return (NULL);

	if (strcasecmp(function, "sha256") == 0)
		fct = HASH_SHA256;

	if (fct == HASH_UNKNOWN) {
		pkg_emit_error("Unsupported hashing function: %s", function);
		return (NULL);
	}

	f = xcalloc(1, sizeof(struct fingerprint));
	f->type = fct;
	strlcpy(f->hash, fp, sizeof(f->hash));

	return (f);
}

static struct fingerprint *
pkg_repo_load_fingerprint(const char *dir, const char *filename)
{
	ucl_object_t *obj = NULL;
	struct ucl_parser *p = NULL;
	char path[MAXPATHLEN];
	struct fingerprint *f = NULL;
	int fd;

	snprintf(path, sizeof(path), "%s/%s", dir, filename);
	fd = openat(ctx.rootfd, RELATIVE_PATH(path), O_RDONLY);
	if (fd == -1) {
		pkg_emit_error("cannot load fingerprints from %s: %s",
				path, strerror(errno));
		return (NULL);
	}

	p = ucl_parser_new(UCL_PARSER_NO_FILEVARS);

	if (!ucl_parser_add_fd(p, fd)) {
		pkg_emit_error("cannot parse fingerprints: %s", ucl_parser_get_error(p));
		ucl_parser_free(p);
		close(fd);
		return (NULL);
	}

	obj = ucl_parser_get_object(p);
	close(fd);

	/* Silently return if obj is NULL */
	if (!obj)
		return(NULL);

	if (obj->type == UCL_OBJECT)
		f = pkg_repo_parse_fingerprint(obj);

	ucl_object_unref(obj);
	ucl_parser_free(p);

	return (f);
}

static int
pkg_repo_load_fingerprints_from_path(const char *path, pkghash **f)
{
	DIR *d;
	int fd;
	struct dirent *ent;
	struct fingerprint *finger = NULL;

	*f = NULL;

	if ((fd = openat(ctx.rootfd, RELATIVE_PATH(path), O_DIRECTORY)) == -1) {
		pkg_emit_error("Error opening the trusted directory %s", path);
		return (EPKG_FATAL);
	}
	if ((d = fdopendir(fd)) == NULL) {
		pkg_emit_error("Error fdopening the trusted directory %s", path);
		return (EPKG_FATAL);
	}

	while ((ent = readdir(d))) {
		if (strcmp(ent->d_name, ".") == 0 ||
		    strcmp(ent->d_name, "..") == 0)
			continue;
		finger = pkg_repo_load_fingerprint(path, ent->d_name);
		if (finger != NULL)
			pkghash_safe_add(*f, finger->hash, finger, NULL);
	}

	closedir(d);

	return (EPKG_OK);
}

int
pkg_repo_load_fingerprints(struct pkg_repo *repo)
{
	char path[MAXPATHLEN];
	struct stat st;

	snprintf(path, sizeof(path), "%s/trusted", pkg_repo_fingerprints(repo));

	if ((pkg_repo_load_fingerprints_from_path(path, &repo->trusted_fp)) != EPKG_OK) {
		pkg_emit_error("Error loading trusted certificates");
		return (EPKG_FATAL);
	}

	if (pkghash_count(repo->trusted_fp) == 0) {
		pkg_emit_error("No trusted certificates");
		return (EPKG_FATAL);
	}

	snprintf(path, sizeof(path), "%s/revoked", pkg_repo_fingerprints(repo));
	/* Absence of revoked certificates is not a fatal error */
	if (fstatat(ctx.rootfd, RELATIVE_PATH(path), &st, 0) != -1) {
		if ((pkg_repo_load_fingerprints_from_path(path, &repo->revoked_fp)) != EPKG_OK) {
			pkg_emit_error("Error loading revoked certificates");
			return (EPKG_FATAL);
		}
	}

	return (EPKG_OK);
}


int
pkg_repo_fetch_package(struct pkg *pkg)
{
	struct pkg_repo *repo;

	if (pkg->repo == NULL) {
		pkg_emit_error("Trying to fetch package without repository");
		return (EPKG_FATAL);
	}

	repo = pkg->repo;
	if (repo->ops->fetch_pkg == NULL) {
		pkg_emit_error("Repository %s does not support fetching", repo->name);
		return (EPKG_FATAL);
	}

	return (repo->ops->fetch_pkg(repo, pkg));
}

int
pkg_repo_mirror_package(struct pkg *pkg, const char *destdir)
{
	struct pkg_repo *repo;

	if (pkg->repo == NULL) {
		pkg_emit_error("Trying to mirror package without repository");
		return (EPKG_FATAL);
	}

	repo = pkg->repo;
	if (repo->ops->mirror_pkg == NULL) {
		pkg_emit_error("Repository %s does not support mirroring", repo->name);
		return (EPKG_FATAL);
	}

	return (repo->ops->mirror_pkg(repo, pkg, destdir));
}

int
pkg_repo_cached_name(struct pkg *pkg, char *dest, size_t destlen)
{
	struct pkg_repo *repo;

	if (pkg->repo == NULL)
		return (EPKG_FATAL);

	repo = pkg->repo;
	if (repo->ops->get_cached_name == NULL)
		return (EPKG_FATAL);

	return (repo->ops->get_cached_name(repo, pkg, dest, destlen));
}
