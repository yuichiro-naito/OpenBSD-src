/*	$OpenBSD: cert.c,v 1.169 2025/06/24 15:47:48 tb Exp $ */
/*
 * Copyright (c) 2022 Theo Buehler <tb@openbsd.org>
 * Copyright (c) 2021 Job Snijders <job@openbsd.org>
 * Copyright (c) 2019 Kristaps Dzonsons <kristaps@bsd.lv>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include <assert.h>
#include <err.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <openssl/asn1.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>

#include "extern.h"

extern ASN1_OBJECT	*bgpsec_oid;	/* id-kp-bgpsec-router Key Purpose */
extern ASN1_OBJECT	*certpol_oid;	/* id-cp-ipAddr-asNumber cert policy */
extern ASN1_OBJECT	*carepo_oid;	/* 1.3.6.1.5.5.7.48.5 (caRepository) */
extern ASN1_OBJECT	*manifest_oid;	/* 1.3.6.1.5.5.7.48.10 (rpkiManifest) */
extern ASN1_OBJECT	*notify_oid;	/* 1.3.6.1.5.5.7.48.13 (rpkiNotify) */

int certid = TALSZ_MAX;

static pthread_rwlock_t	cert_lk = PTHREAD_RWLOCK_INITIALIZER;

/*
 * Append an IP address structure to our list of results.
 * This will also constrain us to having at most one inheritance
 * statement per AFI and also not have overlapping ranges (as prohibited
 * in section 2.2.3.6).
 * It does not make sure that ranges can't coalesce, that is, that any
 * two ranges abut each other.
 * This is warned against in section 2.2.3.6, but doesn't change the
 * semantics of the system.
 * Returns zero on failure (IP overlap) non-zero on success.
 */
static int
append_ip(const char *fn, struct cert_ip *ips, size_t *num_ips,
    const struct cert_ip *ip)
{
	if (!ip_addr_check_overlap(ip, fn, ips, *num_ips, 0))
		return 0;
	ips[(*num_ips)++] = *ip;
	return 1;
}

/*
 * Append an AS identifier structure to our list of results.
 * Makes sure that the identifiers do not overlap or improperly inherit
 * as defined by RFC 3779 section 3.3.
 */
static int
append_as(const char *fn, struct cert_as *ases, size_t *num_ases,
    const struct cert_as *as)
{
	if (!as_check_overlap(as, fn, ases, *num_ases, 0))
		return 0;
	ases[(*num_ases)++] = *as;
	return 1;
}

/*
 * Parse a range of AS identifiers as in 3.2.3.8.
 * Returns zero on failure, non-zero on success.
 */
int
sbgp_as_range(const char *fn, struct cert_as *ases, size_t *num_ases,
    const ASRange *range)
{
	struct cert_as		 as;

	memset(&as, 0, sizeof(struct cert_as));
	as.type = CERT_AS_RANGE;

	if (!as_id_parse(range->min, &as.range.min)) {
		warnx("%s: RFC 3779 section 3.2.3.8 (via RFC 1930): "
		    "malformed AS identifier", fn);
		return 0;
	}

	if (!as_id_parse(range->max, &as.range.max)) {
		warnx("%s: RFC 3779 section 3.2.3.8 (via RFC 1930): "
		    "malformed AS identifier", fn);
		return 0;
	}

	if (as.range.max == as.range.min) {
		warnx("%s: RFC 3379 section 3.2.3.8: ASRange: "
		    "range is singular", fn);
		return 0;
	} else if (as.range.max < as.range.min) {
		warnx("%s: RFC 3379 section 3.2.3.8: ASRange: "
		    "range is out of order", fn);
		return 0;
	}

	return append_as(fn, ases, num_ases, &as);
}

/*
 * Parse an entire 3.2.3.10 integer type.
 */
int
sbgp_as_id(const char *fn, struct cert_as *ases, size_t *num_ases,
    const ASN1_INTEGER *i)
{
	struct cert_as	 as;

	memset(&as, 0, sizeof(struct cert_as));
	as.type = CERT_AS_ID;

	if (!as_id_parse(i, &as.id)) {
		warnx("%s: RFC 3779 section 3.2.3.10 (via RFC 1930): "
		    "malformed AS identifier", fn);
		return 0;
	}
	if (as.id == 0) {
		warnx("%s: RFC 3779 section 3.2.3.10 (via RFC 1930): "
		    "AS identifier zero is reserved", fn);
		return 0;
	}

	return append_as(fn, ases, num_ases, &as);
}

static int
sbgp_as_inherit(const char *fn, struct cert_as *ases, size_t *num_ases)
{
	struct cert_as as;

	memset(&as, 0, sizeof(struct cert_as));
	as.type = CERT_AS_INHERIT;

	return append_as(fn, ases, num_ases, &as);
}

int
sbgp_parse_assysnum(const char *fn, const ASIdentifiers *asidentifiers,
    struct cert_as **out_as, size_t *out_num_ases)
{
	const ASIdOrRanges	*aors = NULL;
	struct cert_as		*as = NULL;
	size_t			 num_ases = 0, num;
	int			 i;

	assert(*out_as == NULL && *out_num_ases == 0);

	if (asidentifiers->rdi != NULL) {
		warnx("%s: RFC 6487 section 4.8.11: autonomousSysNum: "
		    "should not have RDI values", fn);
		goto out;
	}

	if (asidentifiers->asnum == NULL) {
		warnx("%s: RFC 6487 section 4.8.11: autonomousSysNum: "
		    "no AS number resource set", fn);
		goto out;
	}

	switch (asidentifiers->asnum->type) {
	case ASIdentifierChoice_inherit:
		num = 1;
		break;
	case ASIdentifierChoice_asIdsOrRanges:
		aors = asidentifiers->asnum->u.asIdsOrRanges;
		num = sk_ASIdOrRange_num(aors);
		break;
	default:
		warnx("%s: RFC 3779 section 3.2.3.2: ASIdentifierChoice: "
		    "unknown type %d", fn, asidentifiers->asnum->type);
		goto out;
	}

	if (num == 0) {
		warnx("%s: RFC 6487 section 4.8.11: empty asIdsOrRanges", fn);
		goto out;
	}
	if (num >= MAX_AS_SIZE) {
		warnx("%s: too many AS number entries: limit %d",
		    fn, MAX_AS_SIZE);
		goto out;
	}
	as = calloc(num, sizeof(struct cert_as));
	if (as == NULL)
		err(1, NULL);

	if (aors == NULL) {
		if (!sbgp_as_inherit(fn, as, &num_ases))
			goto out;
	}

	for (i = 0; i < sk_ASIdOrRange_num(aors); i++) {
		const ASIdOrRange *aor;

		aor = sk_ASIdOrRange_value(aors, i);
		switch (aor->type) {
		case ASIdOrRange_id:
			if (!sbgp_as_id(fn, as, &num_ases, aor->u.id))
				goto out;
			break;
		case ASIdOrRange_range:
			if (!sbgp_as_range(fn, as, &num_ases, aor->u.range))
				goto out;
			break;
		default:
			warnx("%s: RFC 3779 section 3.2.3.5: ASIdOrRange: "
			    "unknown type %d", fn, aor->type);
			goto out;
		}
	}

	*out_as = as;
	*out_num_ases = num_ases;

	return 1;

 out:
	free(as);

	return 0;
}

/*
 * Parse RFC 6487 4.8.11 X509v3 extension, with syntax documented in RFC
 * 3779 starting in section 3.2.
 * Returns zero on failure, non-zero on success.
 */
static int
sbgp_assysnum(const char *fn, struct cert *cert, X509_EXTENSION *ext)
{
	ASIdentifiers		*asidentifiers = NULL;
	int			 rc = 0;

	if (!X509_EXTENSION_get_critical(ext)) {
		warnx("%s: RFC 6487 section 4.8.11: autonomousSysNum: "
		    "extension not critical", fn);
		goto out;
	}

	if ((asidentifiers = X509V3_EXT_d2i(ext)) == NULL) {
		warnx("%s: RFC 6487 section 4.8.11: autonomousSysNum: "
		    "failed extension parse", fn);
		goto out;
	}

	if (!sbgp_parse_assysnum(fn, asidentifiers, &cert->ases,
	    &cert->num_ases))
		goto out;

	rc = 1;
 out:
	ASIdentifiers_free(asidentifiers);
	return rc;
}

/*
 * Construct a RFC 3779 2.2.3.8 range from its bit string.
 * Returns zero on failure, non-zero on success.
 */
int
sbgp_addr(const char *fn, struct cert_ip *ips, size_t *num_ips, enum afi afi,
    const ASN1_BIT_STRING *bs)
{
	struct cert_ip	ip;

	memset(&ip, 0, sizeof(struct cert_ip));

	ip.afi = afi;
	ip.type = CERT_IP_ADDR;

	if (!ip_addr_parse(bs, afi, fn, &ip.ip)) {
		warnx("%s: RFC 3779 section 2.2.3.8: IPAddress: "
		    "invalid IP address", fn);
		return 0;
	}

	if (!ip_cert_compose_ranges(&ip)) {
		warnx("%s: RFC 3779 section 2.2.3.8: IPAddress: "
		    "IP address range reversed", fn);
		return 0;
	}

	return append_ip(fn, ips, num_ips, &ip);
}

/*
 * Parse RFC 3779 2.2.3.9 range of addresses.
 * Returns zero on failure, non-zero on success.
 */
int
sbgp_addr_range(const char *fn, struct cert_ip *ips, size_t *num_ips,
    enum afi afi, const IPAddressRange *range)
{
	struct cert_ip	ip;

	memset(&ip, 0, sizeof(struct cert_ip));

	ip.afi = afi;
	ip.type = CERT_IP_RANGE;

	if (!ip_addr_parse(range->min, afi, fn, &ip.range.min)) {
		warnx("%s: RFC 3779 section 2.2.3.9: IPAddressRange: "
		    "invalid IP address", fn);
		return 0;
	}

	if (!ip_addr_parse(range->max, afi, fn, &ip.range.max)) {
		warnx("%s: RFC 3779 section 2.2.3.9: IPAddressRange: "
		    "invalid IP address", fn);
		return 0;
	}

	if (!ip_cert_compose_ranges(&ip)) {
		warnx("%s: RFC 3779 section 2.2.3.9: IPAddressRange: "
		    "IP address range reversed", fn);
		return 0;
	}

	return append_ip(fn, ips, num_ips, &ip);
}

static int
sbgp_addr_inherit(const char *fn, struct cert_ip *ips, size_t *num_ips,
    enum afi afi)
{
	struct cert_ip	ip;

	memset(&ip, 0, sizeof(struct cert_ip));

	ip.afi = afi;
	ip.type = CERT_IP_INHERIT;

	return append_ip(fn, ips, num_ips, &ip);
}

int
sbgp_parse_ipaddrblk(const char *fn, const IPAddrBlocks *addrblk,
    struct cert_ip **out_ips, size_t *out_num_ips)
{
	const IPAddressFamily	*af;
	const IPAddressOrRanges	*aors;
	const IPAddressOrRange	*aor;
	enum afi		 afi;
	struct cert_ip		*ips = NULL;
	size_t			 num_ips = 0, num;
	int			 ipv4_seen = 0, ipv6_seen = 0;
	int			 i, j, ipaddrblocksz;

	assert(*out_ips == NULL && *out_num_ips == 0);

	ipaddrblocksz = sk_IPAddressFamily_num(addrblk);
	if (ipaddrblocksz != 1 && ipaddrblocksz != 2) {
		warnx("%s: RFC 6487 section 4.8.10: unexpected number of "
		    "ipAddrBlocks (got %d, expected 1 or 2)",
		    fn, ipaddrblocksz);
		goto out;
	}

	for (i = 0; i < ipaddrblocksz; i++) {
		af = sk_IPAddressFamily_value(addrblk, i);

		switch (af->ipAddressChoice->type) {
		case IPAddressChoice_inherit:
			aors = NULL;
			num = num_ips + 1;
			break;
		case IPAddressChoice_addressesOrRanges:
			aors = af->ipAddressChoice->u.addressesOrRanges;
			num = num_ips + sk_IPAddressOrRange_num(aors);
			break;
		default:
			warnx("%s: RFC 3779: IPAddressChoice: unknown type %d",
			    fn, af->ipAddressChoice->type);
			goto out;
		}
		if (num == num_ips) {
			warnx("%s: RFC 6487 section 4.8.10: "
			    "empty ipAddressesOrRanges", fn);
			goto out;
		}

		if (num >= MAX_IP_SIZE)
			goto out;
		ips = recallocarray(ips, num_ips, num, sizeof(struct cert_ip));
		if (ips == NULL)
			err(1, NULL);

		if (!ip_addr_afi_parse(fn, af->addressFamily, &afi)) {
			warnx("%s: RFC 3779: invalid AFI", fn);
			goto out;
		}

		switch (afi) {
		case AFI_IPV4:
			if (ipv4_seen++ > 0) {
				warnx("%s: RFC 6487 section 4.8.10: "
				    "IPv4 appears twice", fn);
				goto out;
			}
			break;
		case AFI_IPV6:
			if (ipv6_seen++ > 0) {
				warnx("%s: RFC 6487 section 4.8.10: "
				    "IPv6 appears twice", fn);
				goto out;
			}
			break;
		}

		if (aors == NULL) {
			if (!sbgp_addr_inherit(fn, ips, &num_ips, afi))
				goto out;
			continue;
		}

		for (j = 0; j < sk_IPAddressOrRange_num(aors); j++) {
			aor = sk_IPAddressOrRange_value(aors, j);
			switch (aor->type) {
			case IPAddressOrRange_addressPrefix:
				if (!sbgp_addr(fn, ips, &num_ips, afi,
				    aor->u.addressPrefix))
					goto out;
				break;
			case IPAddressOrRange_addressRange:
				if (!sbgp_addr_range(fn, ips, &num_ips, afi,
				    aor->u.addressRange))
					goto out;
				break;
			default:
				warnx("%s: RFC 3779: IPAddressOrRange: "
				    "unknown type %d", fn, aor->type);
				goto out;
			}
		}
	}

	*out_ips = ips;
	*out_num_ips = num_ips;

	return 1;

 out:
	free(ips);

	return 0;
}

/*
 * Parse an sbgp-ipAddrBlock X509 extension, RFC 6487 4.8.10, with
 * syntax documented in RFC 3779 starting in section 2.2.
 * Returns zero on failure, non-zero on success.
 */
static int
sbgp_ipaddrblk(const char *fn, struct cert *cert, X509_EXTENSION *ext)
{
	IPAddrBlocks	*addrblk = NULL;
	int		 rc = 0;

	if (!X509_EXTENSION_get_critical(ext)) {
		warnx("%s: RFC 6487 section 4.8.10: sbgp-ipAddrBlock: "
		    "extension not critical", fn);
		goto out;
	}

	if ((addrblk = X509V3_EXT_d2i(ext)) == NULL) {
		warnx("%s: RFC 6487 section 4.8.10: sbgp-ipAddrBlock: "
		    "failed extension parse", fn);
		goto out;
	}

	if (!sbgp_parse_ipaddrblk(fn, addrblk, &cert->ips, &cert->num_ips))
		goto out;

	if (cert->num_ips == 0) {
		warnx("%s: RFC 6487 section 4.8.10: empty ipAddrBlock", fn);
		goto out;
	}

	rc = 1;
 out:
	IPAddrBlocks_free(addrblk);
	return rc;
}

/*
 * Parse "Subject Information Access" extension for a CA cert,
 * RFC 6487, section 4.8.8.1 and RFC 8182, section 3.2.
 * Returns zero on failure, non-zero on success.
 */
static int
sbgp_sia(const char *fn, struct cert *cert, X509_EXTENSION *ext)
{
	AUTHORITY_INFO_ACCESS	*sia = NULL;
	ACCESS_DESCRIPTION	*ad;
	ASN1_OBJECT		*oid;
	const char		*mftfilename;
	char			*carepo = NULL, *rpkimft = NULL, *notify = NULL;
	int			 i, rc = 0;

	assert(cert->repo == NULL && cert->mft == NULL && cert->notify == NULL);

	if (X509_EXTENSION_get_critical(ext)) {
		warnx("%s: RFC 6487 section 4.8.8: SIA: "
		    "extension not non-critical", fn);
		goto out;
	}

	if ((sia = X509V3_EXT_d2i(ext)) == NULL) {
		warnx("%s: RFC 6487 section 4.8.8: SIA: failed extension parse",
		    fn);
		goto out;
	}

	for (i = 0; i < sk_ACCESS_DESCRIPTION_num(sia); i++) {
		ad = sk_ACCESS_DESCRIPTION_value(sia, i);

		oid = ad->method;

		if (OBJ_cmp(oid, carepo_oid) == 0) {
			if (!x509_location(fn, "SIA: caRepository",
			    ad->location, &carepo))
				goto out;
			if (cert->repo == NULL && strncasecmp(carepo,
			    RSYNC_PROTO, RSYNC_PROTO_LEN) == 0) {
				if (carepo[strlen(carepo) - 1] != '/') {
					char *carepo_tmp;

					if (asprintf(&carepo_tmp, "%s/",
					    carepo) == -1)
						errx(1, NULL);
					free(carepo);
					carepo = carepo_tmp;
				}

				cert->repo = carepo;
				carepo = NULL;
				continue;
			}
			if (verbose)
				warnx("%s: RFC 6487 section 4.8.8: SIA: "
				    "ignoring location %s", fn, carepo);
			free(carepo);
			carepo = NULL;
		} else if (OBJ_cmp(oid, manifest_oid) == 0) {
			if (!x509_location(fn, "SIA: rpkiManifest",
			    ad->location, &rpkimft))
				goto out;
			if (cert->mft == NULL && strncasecmp(rpkimft,
			    RSYNC_PROTO, RSYNC_PROTO_LEN) == 0) {
				cert->mft = rpkimft;
				rpkimft = NULL;
				continue;
			}
			if (verbose)
				warnx("%s: RFC 6487 section 4.8.8: SIA: "
				    "ignoring location %s", fn, rpkimft);
			free(rpkimft);
			rpkimft = NULL;
		} else if (OBJ_cmp(oid, notify_oid) == 0) {
			if (!x509_location(fn, "SIA: rpkiNotify",
			    ad->location, &notify))
				goto out;
			if (strncasecmp(notify, HTTPS_PROTO,
			    HTTPS_PROTO_LEN) != 0) {
				warnx("%s: non-https uri in rpkiNotify: %s",
				    fn, cert->notify);
				free(notify);
				goto out;
			}
			if (cert->notify != NULL) {
				warnx("%s: unexpected rpkiNotify accessMethod",
				    fn);
				free(notify);
				goto out;
			}
			cert->notify = notify;
			notify = NULL;
		} else {
			char buf[128];

			OBJ_obj2txt(buf, sizeof(buf), oid, 0);
			warnx("%s: RFC 6487 section 4.8.8.1: unexpected"
			    " accessMethod: %s", fn, buf);
			goto out;
		}
	}

	if (cert->mft == NULL || cert->repo == NULL) {
		warnx("%s: RFC 6487 section 4.8.8: SIA: missing caRepository "
		    "or rpkiManifest", fn);
		goto out;
	}

	mftfilename = strrchr(cert->mft, '/');
	if (mftfilename == NULL) {
		warnx("%s: SIA: invalid rpkiManifest entry", fn);
		goto out;
	}
	mftfilename++;
	if (!valid_filename(mftfilename, strlen(mftfilename))) {
		warnx("%s: SIA: rpkiManifest filename contains invalid "
		    "characters", fn);
		goto out;
	}

	if (strstr(cert->mft, cert->repo) != cert->mft ||
	    cert->mft + strlen(cert->repo) != mftfilename) {
		warnx("%s: RFC 6487 section 4.8.8: SIA: "
		    "conflicting URIs for caRepository and rpkiManifest", fn);
		goto out;
	}

	if (rtype_from_file_extension(cert->mft) != RTYPE_MFT) {
		warnx("%s: RFC 6487 section 4.8.8: SIA: not an MFT file", fn);
		goto out;
	}

	rc = 1;
 out:
	AUTHORITY_INFO_ACCESS_free(sia);
	return rc;
}

/*
 * Parse the certificate policies extension and check that it follows RFC 7318.
 * Returns zero on failure, non-zero on success.
 */
static int
certificate_policies(const char *fn, struct cert *cert, X509_EXTENSION *ext)
{
	STACK_OF(POLICYINFO)		*policies = NULL;
	POLICYINFO			*policy;
	STACK_OF(POLICYQUALINFO)	*qualifiers;
	POLICYQUALINFO			*qualifier;
	int				 nid;
	int				 rc = 0;

	if (!X509_EXTENSION_get_critical(ext)) {
		warnx("%s: RFC 6487 section 4.8.9: certificatePolicies: "
		    "extension not critical", fn);
		goto out;
	}

	if ((policies = X509V3_EXT_d2i(ext)) == NULL) {
		warnx("%s: RFC 6487 section 4.8.9: certificatePolicies: "
		    "failed extension parse", fn);
		goto out;
	}

	if (sk_POLICYINFO_num(policies) != 1) {
		warnx("%s: RFC 6487 section 4.8.9: certificatePolicies: "
		    "want 1 policy, got %d", fn, sk_POLICYINFO_num(policies));
		goto out;
	}

	policy = sk_POLICYINFO_value(policies, 0);
	assert(policy != NULL && policy->policyid != NULL);

	if (OBJ_cmp(policy->policyid, certpol_oid) != 0) {
		char pbuf[128], cbuf[128];

		OBJ_obj2txt(pbuf, sizeof(pbuf), policy->policyid, 1);
		OBJ_obj2txt(cbuf, sizeof(cbuf), certpol_oid, 1);
		warnx("%s: RFC 7318 section 2: certificatePolicies: "
		    "unexpected OID: %s, want %s", fn, pbuf, cbuf);
		goto out;
	}

	/* Policy qualifiers are optional. If they're absent, we're done. */
	if ((qualifiers = policy->qualifiers) == NULL) {
		rc = 1;
		goto out;
	}

	if (sk_POLICYQUALINFO_num(qualifiers) != 1) {
		warnx("%s: RFC 7318 section 2: certificatePolicies: "
		    "want 1 policy qualifier, got %d", fn,
		    sk_POLICYQUALINFO_num(qualifiers));
		goto out;
	}

	qualifier = sk_POLICYQUALINFO_value(qualifiers, 0);
	assert(qualifier != NULL && qualifier->pqualid != NULL);

	if ((nid = OBJ_obj2nid(qualifier->pqualid)) != NID_id_qt_cps) {
		warnx("%s: RFC 7318 section 2: certificatePolicies: "
		    "want CPS, got %s", fn, nid2str(nid));
		goto out;
	}

	if (verbose > 1 && !filemode)
		warnx("%s: CPS %.*s", fn, qualifier->d.cpsuri->length,
		    qualifier->d.cpsuri->data);

	rc = 1;
 out:
	sk_POLICYINFO_pop_free(policies, POLICYINFO_free);
	return rc;
}

static int
cert_check_subject_and_issuer(const char *fn, const X509 *x)
{
	const X509_NAME *name;

	if ((name = X509_get_subject_name(x)) == NULL) {
		warnx("%s: X509_get_subject_name", fn);
		return 0;
	}
	if (!x509_valid_name(fn, "subject", name))
		return 0;

	if ((name = X509_get_issuer_name(x)) == NULL) {
		warnx("%s: X509_get_issuer_name", fn);
		return 0;
	}
	if (!x509_valid_name(fn, "issuer", name))
		return 0;

	return 1;
}

/*
 * Check the cert's purpose: the cA bit in basic constraints distinguishes
 * between TA/CA and EE/BGPsec router and the key usage bits must match.
 * TAs are self-signed, CAs not self-issued, EEs have no extended key usage,
 * BGPsec router have id-kp-bgpsec-router OID.
 */
static enum cert_purpose
cert_check_purpose(const char *fn, X509 *x)
{
	BASIC_CONSTRAINTS		*bc = NULL;
	EXTENDED_KEY_USAGE		*eku = NULL;
	const X509_EXTENSION		*ku;
	int				 crit, ext_flags, i, is_ca, ku_idx;
	enum cert_purpose		 purpose = CERT_PURPOSE_INVALID;

	if (!x509_cache_extensions(x, fn))
		goto out;

	ext_flags = X509_get_extension_flags(x);

	/* Key usage must be present and critical. KU bits are checked below. */
	if ((ku_idx = X509_get_ext_by_NID(x, NID_key_usage, -1)) < 0) {
		warnx("%s: RFC 6487, section 4.8.4: missing KeyUsage", fn);
		goto out;
	}
	if ((ku = X509_get_ext(x, ku_idx)) == NULL) {
		warnx("%s: RFC 6487, section 4.8.4: missing KeyUsage", fn);
		goto out;
	}
	if (!X509_EXTENSION_get_critical(ku)) {
		warnx("%s: RFC 6487, section 4.8.4: KeyUsage not critical", fn);
		goto out;
	}

	/* This weird API can return 0, 1, 2, 4, 5 but can't error... */
	if ((is_ca = X509_check_ca(x)) > 1) {
		if (is_ca == 4)
			warnx("%s: RFC 6487: sections 4.8.1 and 4.8.4: "
			    "no basic constraints, but keyCertSign set", fn);
		else
			warnx("%s: unexpected legacy certificate", fn);
		goto out;
	}

	if (is_ca) {
		bc = X509_get_ext_d2i(x, NID_basic_constraints, &crit, NULL);
		if (bc == NULL) {
			if (crit != -1)
				warnx("%s: RFC 6487 section 4.8.1: "
				    "error parsing basic constraints", fn);
			else
				warnx("%s: RFC 6487 section 4.8.1: "
				    "missing basic constraints", fn);
			goto out;
		}
		if (crit != 1) {
			warnx("%s: RFC 6487 section 4.8.1: Basic Constraints "
			    "must be marked critical", fn);
			goto out;
		}
		if (bc->pathlen != NULL) {
			warnx("%s: RFC 6487 section 4.8.1: Path Length "
			    "Constraint must be absent", fn);
			goto out;
		}

		if (X509_get_key_usage(x) != (KU_KEY_CERT_SIGN | KU_CRL_SIGN)) {
			warnx("%s: RFC 6487 section 4.8.4: key usage violation",
			    fn);
			goto out;
		}

		if (X509_get_extended_key_usage(x) != UINT32_MAX) {
			warnx("%s: RFC 6487 section 4.8.5: EKU not allowed",
			    fn);
			goto out;
		}

		/*
		 * EXFLAG_SI means that issuer and subject are identical.
		 * EXFLAG_SS is SI plus the AKI is absent or matches the SKI.
		 * Thus, exactly the trust anchors should have EXFLAG_SS set
		 * and we should never see EXFLAG_SI without EXFLAG_SS.
		 */
		if ((ext_flags & EXFLAG_SS) != 0)
			purpose = CERT_PURPOSE_TA;
		else if ((ext_flags & EXFLAG_SI) == 0)
			purpose = CERT_PURPOSE_CA;
		else
			warnx("%s: RFC 6487, section 4.8.3: "
			    "self-issued cert with AKI-SKI mismatch", fn);
		goto out;
	}

	if ((ext_flags & EXFLAG_BCONS) != 0) {
		warnx("%s: Basic Constraints ext in non-CA cert", fn);
		goto out;
	}

	if ((ext_flags & (EXFLAG_SI | EXFLAG_SS)) != 0) {
		warnx("%s: EE cert must not be self-issued or self-signed", fn);
		goto out;
	}

	if (X509_get_key_usage(x) != KU_DIGITAL_SIGNATURE) {
		warnx("%s: RFC 6487 section 4.8.4: KU must be digitalSignature",
		    fn);
		goto out;
	}

	/*
	 * EKU is only defined for BGPsec Router certs and must be absent from
	 * EE certs.
	 */
	eku = X509_get_ext_d2i(x, NID_ext_key_usage, &crit, NULL);
	if (eku == NULL) {
		if (crit != -1)
			warnx("%s: error parsing EKU", fn);
		else
			purpose = CERT_PURPOSE_EE; /* EKU absent */
		goto out;
	}
	if (crit != 0) {
		warnx("%s: EKU: extension must not be marked critical", fn);
		goto out;
	}

	/*
	 * Per RFC 8209, section 3.1.3.2 the id-kp-bgpsec-router OID must be
	 * present and others are allowed, which we don't need to recognize.
	 * This matches RFC 5280, section 4.2.1.12.
	 */
	for (i = 0; i < sk_ASN1_OBJECT_num(eku); i++) {
		if (OBJ_cmp(bgpsec_oid, sk_ASN1_OBJECT_value(eku, i)) == 0) {
			purpose = CERT_PURPOSE_BGPSEC_ROUTER;
			break;
		}
	}

 out:
	BASIC_CONSTRAINTS_free(bc);
	EXTENDED_KEY_USAGE_free(eku);
	return purpose;
}

/*
 * Lightweight version of cert_parse_pre() for EE certs.
 * Parses the two RFC 3779 extensions, and performs some sanity checks.
 * Returns cert on success and NULL on failure.
 */
struct cert *
cert_parse_ee_cert(const char *fn, int talid, X509 *x)
{
	struct cert		*cert;
	X509_EXTENSION		*ext;
	int			 index;

	if ((cert = calloc(1, sizeof(struct cert))) == NULL)
		err(1, NULL);

	if (X509_get_version(x) != 2) {
		warnx("%s: RFC 6487 4.1: X.509 version must be v3", fn);
		goto out;
	}

	if (!cert_check_subject_and_issuer(fn, x))
		goto out;

	if (!x509_cache_extensions(x, fn))
		goto out;

	/*
	 * Check issuance, basic constraints and (extended) key usage bits are
	 * appropriate for an EE cert. Covers RFC 6487, 4.8.1, 4.8.4, 4.8.5.
	 */
	if ((cert->purpose = cert_check_purpose(fn, x)) != CERT_PURPOSE_EE) {
		warnx("%s: expected EE cert, got %s", fn,
		    purpose2str(cert->purpose));
		goto out;
	}

	index = X509_get_ext_by_NID(x, NID_sbgp_ipAddrBlock, -1);
	if ((ext = X509_get_ext(x, index)) != NULL) {
		if (!sbgp_ipaddrblk(fn, cert, ext))
			goto out;
	}

	index = X509_get_ext_by_NID(x, NID_sbgp_autonomousSysNum, -1);
	if ((ext = X509_get_ext(x, index)) != NULL) {
		if (!sbgp_assysnum(fn, cert, ext))
			goto out;
	}

	if (!X509_up_ref(x)) {
		warnx("%s: X509_up_ref failed", fn);
		goto out;
	}

	cert->x509 = x;
	cert->talid = talid;

	if (!constraints_validate(fn, cert))
		goto out;

	return cert;

 out:
	cert_free(cert);
	return NULL;
}

/*
 * Parse and partially validate an RPKI X509 certificate (either a trust
 * anchor or a certificate) as defined in RFC 6487.
 * Returns the parse results or NULL on failure.
 */
struct cert *
cert_parse_pre(const char *fn, const unsigned char *der, size_t len)
{
	struct cert		*cert;
	const unsigned char	*oder;
	size_t			 j;
	int			 i, extsz;
	X509			*x = NULL;
	X509_EXTENSION		*ext = NULL;
	const ASN1_BIT_STRING	*issuer_uid = NULL, *subject_uid = NULL;
	ASN1_OBJECT		*obj;
	EVP_PKEY		*pkey;
	int			 nid, bc, ski, aki, ku, eku, crldp, aia, sia,
				 cp, ip, as;

	nid = bc = ski = aki = ku = eku = crldp = aia = sia = cp = ip = as = 0;

	/* just fail for empty buffers, the warning was printed elsewhere */
	if (der == NULL)
		return NULL;

	if ((cert = calloc(1, sizeof(struct cert))) == NULL)
		err(1, NULL);

	oder = der;
	if ((x = d2i_X509(NULL, &der, len)) == NULL) {
		warnx("%s: d2i_X509", fn);
		goto out;
	}
	if (der != oder + len) {
		warnx("%s: %td bytes trailing garbage", fn, oder + len - der);
		goto out;
	}

	if (!x509_cache_extensions(x, fn))
		goto out;

	if (X509_get_version(x) != 2) {
		warnx("%s: RFC 6487 4.1: X.509 version must be v3", fn);
		goto out;
	}

	if ((nid = X509_get_signature_nid(x)) == NID_undef) {
		warnx("%s: unknown signature type", fn);
		goto out;
	}
	if (experimental && nid == NID_ecdsa_with_SHA256) {
		if (verbose)
			warnx("%s: P-256 support is experimental", fn);
	} else if (nid != NID_sha256WithRSAEncryption) {
		warnx("%s: RFC 7935: wrong signature algorithm %s, want %s",
		    fn, nid2str(nid), LN_sha256WithRSAEncryption);
		goto out;
	}

	/*
	 * Disallowed for CA certs in RFC 5280, 4.1.2.8. Uniqueness of subjects
	 * per RFC 6487, 4.5 makes them meaningless.
	 */
	X509_get0_uids(x, &issuer_uid, &subject_uid);
	if (issuer_uid != NULL || subject_uid != NULL) {
		warnx("%s: issuer or subject unique identifiers not allowed",
		    fn);
		goto out;
	}

	if (!cert_check_subject_and_issuer(fn, x))
		goto out;

	/* Look for X509v3 extensions. */
	if ((extsz = X509_get_ext_count(x)) <= 0) {
		warnx("%s: certificate without X.509v3 extensions", fn);
		goto out;
	}

	for (i = 0; i < extsz; i++) {
		ext = X509_get_ext(x, i);
		assert(ext != NULL);
		obj = X509_EXTENSION_get_object(ext);
		assert(obj != NULL);

		/* The switch is ordered following RFC 6487, section 4.8. */
		switch (nid = OBJ_obj2nid(obj)) {
		case NID_basic_constraints:
			if (bc++ > 0)
				goto dup;
			break;
		case NID_subject_key_identifier:
			if (ski++ > 0)
				goto dup;
			break;
		case NID_authority_key_identifier:
			if (aki++ > 0)
				goto dup;
			break;
		case NID_key_usage:
			if (ku++ > 0)
				goto dup;
			break;
		case NID_ext_key_usage:
			if (eku++ > 0)
				goto dup;
			break;
		case NID_crl_distribution_points:
			if (crldp++ > 0)
				goto dup;
			break;
		case NID_info_access:
			if (aia++ > 0)
				goto dup;
			break;
		case NID_sinfo_access:
			if (sia++ > 0)
				goto dup;
			/*
			 * This will fail for BGPsec certs, but they must omit
			 * this extension anyway (RFC 8209, section 3.1.3.3).
			 */
			if (!sbgp_sia(fn, cert, ext))
				goto out;
			break;
		case NID_certificate_policies:
			if (cp++ > 0)
				goto dup;
			if (!certificate_policies(fn, cert, ext))
				goto out;
			break;
		case NID_sbgp_ipAddrBlock:
			if (ip++ > 0)
				goto dup;
			if (!sbgp_ipaddrblk(fn, cert, ext))
				goto out;
			break;
		case NID_sbgp_autonomousSysNum:
			if (as++ > 0)
				goto dup;
			if (!sbgp_assysnum(fn, cert, ext))
				goto out;
			break;
		default:
			/* unexpected extensions warrant investigation */
			{
				char objn[64];
				OBJ_obj2txt(objn, sizeof(objn), obj, 0);
				warnx("%s: ignoring %s (NID %d)",
				    fn, objn, OBJ_obj2nid(obj));
			}
			break;
		}
	}

	if (!x509_get_aki(x, fn, &cert->aki))
		goto out;
	if (!x509_get_ski(x, fn, &cert->ski))
		goto out;
	if (!x509_get_aia(x, fn, &cert->aia))
		goto out;
	if (!x509_get_crl(x, fn, &cert->crl))
		goto out;
	if (!x509_get_notbefore(x, fn, &cert->notbefore))
		goto out;
	if (!x509_get_notafter(x, fn, &cert->notafter))
		goto out;

	/* Validation on required fields. */
	cert->purpose = cert_check_purpose(fn, x);
	switch (cert->purpose) {
	case CERT_PURPOSE_TA:
		/* XXX - caller should indicate if it expects TA or CA cert */
	case CERT_PURPOSE_CA:
		if ((pkey = X509_get0_pubkey(x)) == NULL) {
			warnx("%s: X509_get0_pubkey failed", fn);
			goto out;
		}
		if (!valid_ca_pkey(fn, pkey))
			goto out;

		if (cert->mft == NULL) {
			warnx("%s: RFC 6487 section 4.8.8: missing SIA", fn);
			goto out;
		}
		if (cert->num_ases == 0 && cert->num_ips == 0) {
			warnx("%s: missing IP or AS resources", fn);
			goto out;
		}
		break;
	case CERT_PURPOSE_BGPSEC_ROUTER:
		cert->pubkey = x509_get_pubkey(x, fn);
		if (cert->pubkey == NULL) {
			warnx("%s: x509_get_pubkey failed", fn);
			goto out;
		}
		if (cert->num_ips > 0) {
			warnx("%s: unexpected IP resources in BGPsec cert", fn);
			goto out;
		}
		for (j = 0; j < cert->num_ases; j++) {
			if (cert->ases[j].type == CERT_AS_INHERIT) {
				warnx("%s: inherit elements not allowed in EE"
				    " cert", fn);
				goto out;
			}
		}
		if (sia) {
			warnx("%s: unexpected SIA extension in BGPsec cert",
			    fn);
			goto out;
		}
		break;
	case CERT_PURPOSE_EE:
		warnx("%s: unexpected EE cert", fn);
		goto out;
	default:
		warnx("%s: x509_get_purpose failed in %s", fn, __func__);
		goto out;
	}

	if (cert->ski == NULL) {
		warnx("%s: RFC 6487 section 8.4.2: missing SKI", fn);
		goto out;
	}

	cert->x509 = x;
	return cert;

 dup:
	warnx("%s: RFC 5280 section 4.2: duplicate extension: %s", fn,
	    nid2str(nid));
 out:
	cert_free(cert);
	X509_free(x);
	return NULL;
}

struct cert *
cert_parse(const char *fn, struct cert *p)
{
	if (p == NULL)
		return NULL;

	if (p->aki == NULL) {
		warnx("%s: RFC 6487 section 8.4.2: "
		    "non-trust anchor missing AKI", fn);
		goto badcert;
	}
	if (strcmp(p->aki, p->ski) == 0) {
		warnx("%s: RFC 6487 section 8.4.2: "
		    "non-trust anchor AKI may not match SKI", fn);
		goto badcert;
	}
	if (p->aia == NULL) {
		warnx("%s: RFC 6487 section 8.4.7: AIA: extension missing", fn);
		goto badcert;
	}
	if (p->crl == NULL) {
		warnx("%s: RFC 6487 section 4.8.6: CRL: "
		    "no CRL distribution point extension", fn);
		goto badcert;
	}
	return p;

 badcert:
	cert_free(p);
	return NULL;
}

struct cert *
ta_parse(const char *fn, struct cert *p, const unsigned char *pkey,
    size_t pkeysz)
{
	EVP_PKEY	*pk, *opk;
	time_t		 now = get_current_time();

	if (p == NULL)
		return NULL;

	/* first check pubkey against the one from the TAL */
	pk = d2i_PUBKEY(NULL, &pkey, pkeysz);
	if (pk == NULL) {
		warnx("%s: RFC 6487 (trust anchor): bad TAL pubkey", fn);
		goto badcert;
	}
	if ((opk = X509_get0_pubkey(p->x509)) == NULL) {
		warnx("%s: RFC 6487 (trust anchor): missing pubkey", fn);
		goto badcert;
	}
	if (EVP_PKEY_cmp(pk, opk) != 1) {
		warnx("%s: RFC 6487 (trust anchor): "
		    "pubkey does not match TAL pubkey", fn);
		goto badcert;
	}

	if (p->notbefore > now) {
		warnx("%s: certificate not yet valid", fn);
		goto badcert;
	}
	if (p->notafter < now) {
		warnx("%s: certificate has expired", fn);
		goto badcert;
	}
	if (p->aki != NULL && strcmp(p->aki, p->ski)) {
		warnx("%s: RFC 6487 section 4.8.3: "
		    "trust anchor AKI, if specified, must match SKI", fn);
		goto badcert;
	}
	if (p->aia != NULL) {
		warnx("%s: RFC 6487 section 4.8.7: "
		    "trust anchor must not have AIA", fn);
		goto badcert;
	}
	if (p->crl != NULL) {
		warnx("%s: RFC 6487 section 4.8.6: "
		    "trust anchor may not specify CRL resource", fn);
		goto badcert;
	}
	if (p->purpose != CERT_PURPOSE_TA) {
		warnx("%s: expected trust anchor purpose, got %s", fn,
		    purpose2str(p->purpose));
		goto badcert;
	}
	/*
	 * Do not replace with a <= 0 check since OpenSSL 3 broke that:
	 * https://github.com/openssl/openssl/issues/24575
	 */
	if (X509_verify(p->x509, pk) != 1) {
		warnx("%s: failed to verify signature", fn);
		goto badcert;
	}
	if (x509_any_inherits(p->x509)) {
		warnx("%s: Trust anchor IP/AS resources may not inherit", fn);
		goto badcert;
	}

	EVP_PKEY_free(pk);
	return p;

 badcert:
	EVP_PKEY_free(pk);
	cert_free(p);
	return NULL;
}

/*
 * Free parsed certificate contents.
 * Passing NULL is a noop.
 */
void
cert_free(struct cert *p)
{
	if (p == NULL)
		return;

	free(p->crl);
	free(p->repo);
	free(p->path);
	free(p->mft);
	free(p->notify);
	free(p->ips);
	free(p->ases);
	free(p->aia);
	free(p->aki);
	free(p->ski);
	free(p->pubkey);
	X509_free(p->x509);
	free(p);
}

/*
 * Write certificate parsed content into buffer.
 * See cert_read() for the other side of the pipe.
 */
void
cert_buffer(struct ibuf *b, const struct cert *p)
{
	io_simple_buffer(b, &p->notafter, sizeof(p->notafter));
	io_simple_buffer(b, &p->purpose, sizeof(p->purpose));
	io_simple_buffer(b, &p->talid, sizeof(p->talid));
	io_simple_buffer(b, &p->certid, sizeof(p->certid));
	io_simple_buffer(b, &p->repoid, sizeof(p->repoid));
	io_simple_buffer(b, &p->num_ips, sizeof(p->num_ips));
	io_simple_buffer(b, &p->num_ases, sizeof(p->num_ases));

	io_simple_buffer(b, p->ips, p->num_ips * sizeof(p->ips[0]));
	io_simple_buffer(b, p->ases, p->num_ases * sizeof(p->ases[0]));

	io_str_buffer(b, p->path);
	io_str_buffer(b, p->mft);
	io_str_buffer(b, p->notify);
	io_str_buffer(b, p->repo);
	io_str_buffer(b, p->crl);
	io_str_buffer(b, p->aia);
	io_str_buffer(b, p->aki);
	io_str_buffer(b, p->ski);
	io_str_buffer(b, p->pubkey);
}

/*
 * Allocate and read parsed certificate content from descriptor.
 * The pointer must be freed with cert_free().
 * Always returns a valid pointer.
 */
struct cert *
cert_read(struct ibuf *b)
{
	struct cert	*p;

	if ((p = calloc(1, sizeof(struct cert))) == NULL)
		err(1, NULL);

	io_read_buf(b, &p->notafter, sizeof(p->notafter));
	io_read_buf(b, &p->purpose, sizeof(p->purpose));
	io_read_buf(b, &p->talid, sizeof(p->talid));
	io_read_buf(b, &p->certid, sizeof(p->certid));
	io_read_buf(b, &p->repoid, sizeof(p->repoid));
	io_read_buf(b, &p->num_ips, sizeof(p->num_ips));
	io_read_buf(b, &p->num_ases, sizeof(p->num_ases));

	if (p->num_ips > 0) {
		if ((p->ips = calloc(p->num_ips, sizeof(p->ips[0]))) == NULL)
			err(1, NULL);
		io_read_buf(b, p->ips, p->num_ips * sizeof(p->ips[0]));
	}

	if (p->num_ases > 0) {
		if ((p->ases = calloc(p->num_ases, sizeof(p->ases[0]))) == NULL)
			err(1, NULL);
		io_read_buf(b, p->ases, p->num_ases * sizeof(p->ases[0]));
	}

	io_read_str(b, &p->path);
	io_read_str(b, &p->mft);
	io_read_str(b, &p->notify);
	io_read_str(b, &p->repo);
	io_read_str(b, &p->crl);
	io_read_str(b, &p->aia);
	io_read_str(b, &p->aki);
	io_read_str(b, &p->ski);
	io_read_str(b, &p->pubkey);

	assert(p->mft != NULL || p->purpose == CERT_PURPOSE_BGPSEC_ROUTER);
	assert(p->ski);
	return p;
}

static inline int
authcmp(struct auth *a, struct auth *b)
{
	if (a->cert->certid > b->cert->certid)
		return 1;
	if (a->cert->certid < b->cert->certid)
		return -1;
	return 0;
}

RB_GENERATE_STATIC(auth_tree, auth, entry, authcmp);

void
auth_tree_free(struct auth_tree *auths)
{
	struct auth	*auth, *tauth;

	RB_FOREACH_SAFE(auth, auth_tree, auths, tauth) {
		RB_REMOVE(auth_tree, auths, auth);
		cert_free(auth->cert);
		free(auth);
	}
}

struct auth *
auth_find(struct auth_tree *auths, int id)
{
	struct auth a, *f;
	struct cert c;
	int error;

	c.certid = id;
	a.cert = &c;

	if ((error = pthread_rwlock_rdlock(&cert_lk)) != 0)
		errx(1, "pthread_rwlock_rdlock: %s", strerror(error));
	f = RB_FIND(auth_tree, auths, &a);
	if ((error = pthread_rwlock_unlock(&cert_lk)) != 0)
		errx(1, "pthread_rwlock_unlock: %s", strerror(error));
	return f;
}

struct auth *
auth_insert(const char *fn, struct auth_tree *auths, struct cert *cert,
    struct auth *issuer)
{
	struct auth *na;
	int error;

	na = calloc(1, sizeof(*na));
	if (na == NULL)
		err(1, NULL);

	if ((error = pthread_rwlock_wrlock(&cert_lk)) != 0)
		errx(1, "pthread_rwlock_wrlock: %s", strerror(error));
	if (issuer == NULL) {
		cert->certid = cert->talid;
	} else {
		cert->certid = ++certid;
		if (certid > CERTID_MAX) {
			if (certid == CERTID_MAX + 1)
				warnx("%s: too many certificates in store", fn);
			goto fail;
		}
		na->depth = issuer->depth + 1;
	}

	if (na->depth >= MAX_CERT_DEPTH) {
		warnx("%s: maximum certificate chain depth exhausted", fn);
		goto fail;
	}

	na->issuer = issuer;
	na->cert = cert;
	na->any_inherits = x509_any_inherits(cert->x509);

	if (RB_INSERT(auth_tree, auths, na) != NULL)
		errx(1, "auth tree corrupted");

	if ((error = pthread_rwlock_unlock(&cert_lk)) != 0)
		errx(1, "pthread_rwlock_unlock: %s", strerror(error));

	return na;

 fail:
	if ((error = pthread_rwlock_unlock(&cert_lk)) != 0)
		errx(1, "pthread_rwlock_unlock: %s", strerror(error));
	free(na);
	return NULL;
}

static void
insert_brk(struct brk_tree *tree, struct cert *cert, int asid)
{
	struct brk	*b, *found;

	if ((b = calloc(1, sizeof(*b))) == NULL)
		err(1, NULL);

	b->asid = asid;
	b->expires = cert->notafter;
	b->talid = cert->talid;
	if ((b->ski = strdup(cert->ski)) == NULL)
		err(1, NULL);
	if ((b->pubkey = strdup(cert->pubkey)) == NULL)
		err(1, NULL);

	/*
	 * Check if a similar BRK already exists in the tree. If the found BRK
	 * expires sooner, update it to this BRK's later expiry moment.
	 */
	if ((found = RB_INSERT(brk_tree, tree, b)) != NULL) {
		if (found->expires < b->expires) {
			found->expires = b->expires;
			found->talid = b->talid;
		}
		free(b->ski);
		free(b->pubkey);
		free(b);
	}
}

/*
 * Add each BGPsec Router Key into the BRK tree.
 */
void
cert_insert_brks(struct brk_tree *tree, struct cert *cert)
{
	size_t		 i, asid;

	for (i = 0; i < cert->num_ases; i++) {
		switch (cert->ases[i].type) {
		case CERT_AS_ID:
			insert_brk(tree, cert, cert->ases[i].id);
			break;
		case CERT_AS_RANGE:
			for (asid = cert->ases[i].range.min;
			    asid <= cert->ases[i].range.max; asid++)
				insert_brk(tree, cert, asid);
			break;
		default:
			warnx("invalid AS identifier type");
			continue;
		}
	}
}

static inline int
brkcmp(struct brk *a, struct brk *b)
{
	int rv;

	if (a->asid > b->asid)
		return 1;
	if (a->asid < b->asid)
		return -1;

	rv = strcmp(a->ski, b->ski);
	if (rv > 0)
		return 1;
	if (rv < 0)
		return -1;

	return strcmp(a->pubkey, b->pubkey);
}

RB_GENERATE(brk_tree, brk, entry, brkcmp);

/*
 * Add each CA cert into the non-functional CA tree.
 */
void
cert_insert_nca(struct nca_tree *tree, const struct cert *cert, struct repo *rp)
{
	struct nonfunc_ca *nca;

	if ((nca = calloc(1, sizeof(*nca))) == NULL)
		err(1, NULL);
	if ((nca->location = strdup(cert->path)) == NULL)
		err(1, NULL);
	if ((nca->carepo = strdup(cert->repo)) == NULL)
		err(1, NULL);
	if ((nca->mfturi = strdup(cert->mft)) == NULL)
		err(1, NULL);
	if ((nca->ski = strdup(cert->ski)) == NULL)
		err(1, NULL);
	nca->certid = cert->certid;
	nca->talid = cert->talid;

	if (RB_INSERT(nca_tree, tree, nca) != NULL)
		errx(1, "non-functional CA tree corrupted");
	repo_stat_inc(rp, nca->talid, RTYPE_CER, STYPE_NONFUNC);
}

void
cert_remove_nca(struct nca_tree *tree, int cid, struct repo *rp)
{
	struct nonfunc_ca *found, needle = { .certid = cid };

	if ((found = RB_FIND(nca_tree, tree, &needle)) != NULL) {
		RB_REMOVE(nca_tree, tree, found);
		repo_stat_inc(rp, found->talid, RTYPE_CER, STYPE_FUNC);
		free(found->location);
		free(found->carepo);
		free(found->mfturi);
		free(found->ski);
		free(found);
	}
}

static inline int
ncacmp(const struct nonfunc_ca *a, const struct nonfunc_ca *b)
{
	if (a->certid < b->certid)
		return -1;
	if (a->certid > b->certid)
		return 1;
	return 0;
}

RB_GENERATE(nca_tree, nonfunc_ca, entry, ncacmp);
