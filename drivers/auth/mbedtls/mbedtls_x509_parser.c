/*
 * Copyright (c) 2015, ARM Limited and Contributors. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * Redistributions of source code must retain the above copyright notice, this
 * list of conditions and the following disclaimer.
 *
 * Redistributions in binary form must reproduce the above copyright notice,
 * this list of conditions and the following disclaimer in the documentation
 * and/or other materials provided with the distribution.
 *
 * Neither the name of ARM nor the names of its contributors may be used
 * to endorse or promote products derived from this software without specific
 * prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

/*
 * X509 parser based on PolarSSL
 *
 * This module implements functions to check the integrity of a X509v3
 * certificate ASN.1 structure and extract authentication parameters from the
 * extensions field, such as an image hash or a public key.
 */

#include <assert.h>
#include <img_parser_mod.h>
#include <mbedtls_common.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* mbedTLS headers */
#include <polarssl/asn1.h>
#include <polarssl/oid.h>
#include <polarssl/platform.h>

/* Maximum OID string length ("a.b.c.d.e.f ...") */
#define MAX_OID_STR_LEN			64

#define LIB_NAME	"mbedTLS X509v3"

/* Temporary variables to speed up the authentication parameters search. These
 * variables are assigned once during the integrity check and used any time an
 * authentication parameter is requested, so we do not have to parse the image
 * again */
static asn1_buf tbs;
static asn1_buf v3_ext;
static asn1_buf pk;
static asn1_buf sig_alg;
static asn1_buf signature;

/*
 * Get X509v3 extension
 *
 * Global variable 'v3_ext' must point to the extensions region
 * in the certificate. No need to check for errors since the image has passed
 * the integrity check.
 */
static int get_ext(const char *oid, void **ext, unsigned int *ext_len)
{
	int oid_len;
	size_t len;
	unsigned char *end_ext_data, *end_ext_octet;
	unsigned char *p;
	const unsigned char *end;
	char oid_str[MAX_OID_STR_LEN];
	asn1_buf extn_oid;
	int is_critical;

	assert(oid != NULL);

	p = v3_ext.p;
	end = v3_ext.p + v3_ext.len;

	asn1_get_tag(&p, end, &len, ASN1_CONSTRUCTED | ASN1_SEQUENCE);

	while (p < end) {
		memset(&extn_oid, 0x0, sizeof(extn_oid));
		is_critical = 0; /* DEFAULT FALSE */

		asn1_get_tag(&p, end, &len, ASN1_CONSTRUCTED | ASN1_SEQUENCE);
		end_ext_data = p + len;

		/* Get extension ID */
		extn_oid.tag = *p;
		asn1_get_tag(&p, end, &extn_oid.len, ASN1_OID);
		extn_oid.p = p;
		p += extn_oid.len;

		/* Get optional critical */
		asn1_get_bool(&p, end_ext_data, &is_critical);

		/* Extension data */
		asn1_get_tag(&p, end_ext_data, &len, ASN1_OCTET_STRING);
		end_ext_octet = p + len;

		/* Detect requested extension */
		oid_len = oid_get_numeric_string(oid_str,
				MAX_OID_STR_LEN, &extn_oid);
		if (oid_len == POLARSSL_ERR_OID_BUF_TOO_SMALL) {
			return IMG_PARSER_ERR;
		}
		if ((oid_len == strlen(oid_str)) && !strcmp(oid, oid_str)) {
			*ext = (void *)p;
			*ext_len = (unsigned int)len;
			return IMG_PARSER_OK;
		}

		/* Next */
		p = end_ext_octet;
	}

	return IMG_PARSER_ERR_NOT_FOUND;
}


/*
 * Check the integrity of the certificate ASN.1 structure.
 * Extract the relevant data that will be used later during authentication.
 */
static int cert_parse(void *img, unsigned int img_len)
{
	int ret, is_critical;
	size_t len;
	unsigned char *p, *end, *crt_end;
	asn1_buf sig_alg1, sig_alg2;

	p = (unsigned char *)img;
	len = img_len;
	end = p + len;

	/*
	 * Certificate  ::=  SEQUENCE  {
	 *      tbsCertificate       TBSCertificate,
	 *      signatureAlgorithm   AlgorithmIdentifier,
	 *      signatureValue       BIT STRING  }
	 */
	ret = asn1_get_tag(&p, end, &len, ASN1_CONSTRUCTED | ASN1_SEQUENCE);
	if (ret != 0) {
		return IMG_PARSER_ERR_FORMAT;
	}

	if (len > (size_t)(end - p)) {
		return IMG_PARSER_ERR_FORMAT;
	}
	crt_end = p + len;

	/*
	 * TBSCertificate  ::=  SEQUENCE  {
	 */
	tbs.p = p;
	ret = asn1_get_tag(&p, end, &len, ASN1_CONSTRUCTED | ASN1_SEQUENCE);
	if (ret != 0) {
		return IMG_PARSER_ERR_FORMAT;
	}
	end = p + len;
	tbs.len = end - tbs.p;

	/*
	 * Version  ::=  INTEGER  {  v1(0), v2(1), v3(2)  }
	 */
	ret = asn1_get_tag(&p, end, &len,
			ASN1_CONTEXT_SPECIFIC | ASN1_CONSTRUCTED | 0);
	if (ret != 0) {
		return IMG_PARSER_ERR_FORMAT;
	}
	p += len;

	/*
	 * CertificateSerialNumber  ::=  INTEGER
	 */
	ret = asn1_get_tag(&p, end, &len, ASN1_INTEGER);
	if (ret != 0) {
		return IMG_PARSER_ERR_FORMAT;
	}
	p += len;

	/*
	 * signature            AlgorithmIdentifier
	 */
	sig_alg1.p = p;
	ret = asn1_get_tag(&p, end, &len, ASN1_CONSTRUCTED | ASN1_SEQUENCE);
	if (ret != 0) {
		return IMG_PARSER_ERR_FORMAT;
	}
	if ((end - p) < 1) {
		return IMG_PARSER_ERR_FORMAT;
	}
	sig_alg1.len = (p + len) - sig_alg1.p;
	p += len;

	/*
	 * issuer               Name
	 */
	ret = asn1_get_tag(&p, end, &len, ASN1_CONSTRUCTED | ASN1_SEQUENCE);
	if (ret != 0) {
		return IMG_PARSER_ERR_FORMAT;
	}
	p += len;

	/*
	 * Validity ::= SEQUENCE {
	 *      notBefore      Time,
	 *      notAfter       Time }
	 *
	 */
	ret = asn1_get_tag(&p, end, &len, ASN1_CONSTRUCTED | ASN1_SEQUENCE);
	if (ret != 0) {
		return IMG_PARSER_ERR_FORMAT;
	}
	p += len;

	/*
	 * subject              Name
	 */
	ret = asn1_get_tag(&p, end, &len, ASN1_CONSTRUCTED | ASN1_SEQUENCE);
	if (ret != 0) {
		return IMG_PARSER_ERR_FORMAT;
	}
	p += len;

	/*
	 * SubjectPublicKeyInfo
	 */
	pk.p = p;
	ret = asn1_get_tag(&p, end, &len, ASN1_CONSTRUCTED | ASN1_SEQUENCE);
	if (ret != 0) {
		return IMG_PARSER_ERR_FORMAT;
	}
	pk.len = (p + len) - pk.p;
	p += len;

	/*
	 * issuerUniqueID  [1]  IMPLICIT UniqueIdentifier OPTIONAL,
	 */
	ret = asn1_get_tag(&p, end, &len,
			ASN1_CONTEXT_SPECIFIC | ASN1_CONSTRUCTED | 1);
	if (ret != 0) {
		if (ret != POLARSSL_ERR_ASN1_UNEXPECTED_TAG) {
			return IMG_PARSER_ERR_FORMAT;
		}
	} else {
		p += len;
	}

	/*
	 * subjectUniqueID [2]  IMPLICIT UniqueIdentifier OPTIONAL,
	 */
	ret = asn1_get_tag(&p, end, &len,
			ASN1_CONTEXT_SPECIFIC | ASN1_CONSTRUCTED | 2);
	if (ret != 0) {
		if (ret != POLARSSL_ERR_ASN1_UNEXPECTED_TAG) {
			return IMG_PARSER_ERR_FORMAT;
		}
	} else {
		p += len;
	}

	/*
	 * extensions      [3]  EXPLICIT Extensions OPTIONAL
	 */
	ret = asn1_get_tag(&p, end, &len,
			ASN1_CONTEXT_SPECIFIC | ASN1_CONSTRUCTED | 3);
	if (ret != 0) {
		return IMG_PARSER_ERR_FORMAT;
	}

	/*
	 * Extensions  ::=  SEQUENCE SIZE (1..MAX) OF Extension
	 */
	v3_ext.p = p;
	ret = asn1_get_tag(&p, end, &len, ASN1_CONSTRUCTED | ASN1_SEQUENCE);
	if (ret != 0) {
		return IMG_PARSER_ERR_FORMAT;
	}
	v3_ext.len = (p + len) - v3_ext.p;

	/*
	 * Check extensions integrity
	 */
	while (p < end) {
		ret = asn1_get_tag(&p, end, &len,
				ASN1_CONSTRUCTED | ASN1_SEQUENCE);
		if (ret != 0) {
			return IMG_PARSER_ERR_FORMAT;
		}

		/* Get extension ID */
		ret = asn1_get_tag(&p, end, &len, ASN1_OID);
		if (ret != 0) {
			return IMG_PARSER_ERR_FORMAT;
		}
		p += len;

		/* Get optional critical */
		ret = asn1_get_bool(&p, end, &is_critical);
		if ((ret != 0) && (ret != POLARSSL_ERR_ASN1_UNEXPECTED_TAG)) {
			return IMG_PARSER_ERR_FORMAT;
		}

		/* Data should be octet string type */
		ret = asn1_get_tag(&p, end, &len, ASN1_OCTET_STRING);
		if (ret != 0) {
			return IMG_PARSER_ERR_FORMAT;
		}
		p += len;
	}

	if (p != end) {
		return IMG_PARSER_ERR_FORMAT;
	}

	end = crt_end;

	/*
	 *  }
	 *  -- end of TBSCertificate
	 *
	 *  signatureAlgorithm   AlgorithmIdentifier
	 */
	sig_alg2.p = p;
	ret = asn1_get_tag(&p, end, &len, ASN1_CONSTRUCTED | ASN1_SEQUENCE);
	if (ret != 0) {
		return IMG_PARSER_ERR_FORMAT;
	}
	if ((end - p) < 1) {
		return IMG_PARSER_ERR_FORMAT;
	}
	sig_alg2.len = (p + len) - sig_alg2.p;
	p += len;

	/* Compare both signature algorithms */
	if (sig_alg1.len != sig_alg2.len) {
		return IMG_PARSER_ERR_FORMAT;
	}
	if (0 != memcmp(sig_alg1.p, sig_alg2.p, sig_alg1.len)) {
		return IMG_PARSER_ERR_FORMAT;
	}
	memcpy(&sig_alg, &sig_alg1, sizeof(sig_alg));

	/*
	 * signatureValue       BIT STRING
	 */
	signature.p = p;
	ret = asn1_get_tag(&p, end, &len, ASN1_BIT_STRING);
	if (ret != 0) {
		return IMG_PARSER_ERR_FORMAT;
	}
	signature.len = (p + len) - signature.p;
	p += len;

	/* Check certificate length */
	if (p != end) {
		return IMG_PARSER_ERR_FORMAT;
	}

	return IMG_PARSER_OK;
}


/* Exported functions */

static void init(void)
{
	mbedtls_init();
}

static int check_integrity(void *img, unsigned int img_len)
{
	return cert_parse(img, img_len);
}

/*
 * Extract an authentication parameter from an X509v3 certificate
 */
static int get_auth_param(const auth_param_type_desc_t *type_desc,
		void *img, unsigned int img_len,
		void **param, unsigned int *param_len)
{
	int rc = IMG_PARSER_OK;

	/* We do not use img because the check_integrity function has already
	 * extracted the relevant data (v3_ext, pk, sig_alg, etc) */

	switch (type_desc->type) {
	case AUTH_PARAM_RAW_DATA:
		/* Data to be signed */
		*param = (void *)tbs.p;
		*param_len = (unsigned int)tbs.len;
		break;
	case AUTH_PARAM_HASH:
		/* All these parameters are included as X509v3 extensions */
		rc = get_ext(type_desc->cookie, param, param_len);
		break;
	case AUTH_PARAM_PUB_KEY:
		if (type_desc->cookie != 0) {
			/* Get public key from extension */
			rc = get_ext(type_desc->cookie, param, param_len);
		} else {
			/* Get the subject public key */
			*param = (void *)pk.p;
			*param_len = (unsigned int)pk.len;
		}
		break;
	case AUTH_PARAM_SIG_ALG:
		/* Get the certificate signature algorithm */
		*param = (void *)sig_alg.p;
		*param_len = (unsigned int)sig_alg.len;
		break;
	case AUTH_PARAM_SIG:
		/* Get the certificate signature */
		*param = (void *)signature.p;
		*param_len = (unsigned int)signature.len;
		break;
	default:
		rc = IMG_PARSER_ERR_NOT_FOUND;
		break;
	}

	return rc;
}

REGISTER_IMG_PARSER_LIB(IMG_CERT, LIB_NAME, init, \
		       check_integrity, get_auth_param);
