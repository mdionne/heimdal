/*-
 * Copyright (c) 2005 Doug Rabson
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 *	$FreeBSD: src/lib/libgssapi/gss_import_name.c,v 1.1 2005/12/29 14:40:20 dfr Exp $
 */

#include "mech_locl.h"

static OM_uint32
_gss_import_export_name(OM_uint32 *minor_status,
    const gss_buffer_t input_name_buffer,
    const gss_OID name_type,
    gss_name_t *output_name)
{
	OM_uint32 major_status;
	unsigned char *p = input_name_buffer->value;
	size_t len = input_name_buffer->length;
	size_t t;
	gss_OID_desc mech_oid;
	gssapi_mech_interface m;
	struct _gss_name *name;
	gss_name_t new_canonical_name;
	int composite = 0;

	*minor_status = 0;
	*output_name = 0;

	/*
	 * Make sure that TOK_ID is {4, 1}.
	 */
	if (len < 2)
		return (GSS_S_BAD_NAME);
	if (p[0] != 4)
		return (GSS_S_BAD_NAME);
	switch (p[1]) {
	case 1:	/* non-composite name */
		break;
	case 2:	/* composite name */
		composite = 1;
		break;
	default:
		return (GSS_S_BAD_NAME);
	}
	p += 2;
	len -= 2;

        /*
         * If the name token is a composite token (TOK_ID 0x04 0x02) then per
         * RFC6680 everything after that is implementation-specific.  This
         * mech-glue is pluggable however, so we need the format of the rest of
         * the header to be stable, otherwise we couldn't reliably determine
         * what mechanism the token is for and we'd have to try all of them.
         *
         * So... we keep the same format for the exported composite name token
         * as for normal exported name tokens (see RFC2743, section 3.2), with
         * the TOK_ID 0x04 0x02, but only up to the mechanism OID.  We don't
         * enforce that there be a NAME_LEN in the exported composite name
         * token, or that it match the length of the remainder of the token.
         *
         * FYI, at least one out-of-tree mechanism implements exported
         * composite name tokens as the same as exported name tokens with
         * attributes appended and the NAME_LEN not modified to match.
         */

	/*
	 * Get the mech length and the name length and sanity
	 * check the size of of the buffer.
	 */
	if (len < 2)
		return (GSS_S_BAD_NAME);
	t = (p[0] << 8) + p[1];
	p += 2;
	len -= 2;

	/*
	 * Check the DER encoded OID to make sure it agrees with the
	 * length we just decoded.
	 */
	if (p[0] != 6)		/* 6=OID */
		return (GSS_S_BAD_NAME);
	p++;
	len--;
	t--;
	if (p[0] & 0x80) {
		int digits = p[0];
		p++;
		len--;
		t--;
		mech_oid.length = 0;
		while (digits--) {
			mech_oid.length = (mech_oid.length << 8) | p[0];
			p++;
			len--;
			t--;
		}
	} else {
		mech_oid.length = p[0];
		p++;
		len--;
		t--;
	}
	if (mech_oid.length != t)
		return (GSS_S_BAD_NAME);

	mech_oid.elements = p;

        if (!composite) {
                if (len < t + 4)
                        return (GSS_S_BAD_NAME);
                p += t;
                len -= t;

                t = (p[0] << 24) | (p[1] << 16) | (p[2] << 8) | p[3];
                p += 4;
                len -= 4;

                if (len != t)
                        return (GSS_S_BAD_NAME);
        }

	m = __gss_get_mechanism(&mech_oid);
	if (!m || !m->gm_import_name)
		return (GSS_S_BAD_MECH);

	/*
	 * Ask the mechanism to import the name.
	 */
	major_status = m->gm_import_name(minor_status,
            input_name_buffer, name_type, &new_canonical_name);
	if (major_status != GSS_S_COMPLETE) {
		_gss_mg_error(m, *minor_status);
		return major_status;
	}

	/*
	 * Now we make a new name and mark it as an MN.
	 */
	name = _gss_create_name(new_canonical_name, m);
	if (!name) {
		m->gm_release_name(minor_status, &new_canonical_name);
		return (GSS_S_FAILURE);
	}

	*output_name = (gss_name_t) name;

	*minor_status = 0;
	return (GSS_S_COMPLETE);
}

/**
 * Convert a GGS-API name from contiguous string to internal form.
 *
 * Type of name and their format:
 * - GSS_C_NO_OID
 * - GSS_C_NT_USER_NAME
 * - GSS_C_NT_HOSTBASED_SERVICE
 * - GSS_C_NT_EXPORT_NAME
 * - GSS_C_NT_COMPOSITE_EXPORT
 * - GSS_C_NT_ANONYMOUS
 * - GSS_KRB5_NT_PRINCIPAL_NAME
 *
 * @sa gss_export_name(), @ref internalVSmechname.
 *
 * @param minor_status       minor status code
 * @param input_name_buffer  import name buffer
 * @param input_name_type    type of the import name buffer
 * @param output_name        the resulting type, release with
 *        gss_release_name(), independent of input_name
 *
 * @returns a gss_error code, see gss_display_status() about printing
 *        the error code.
 *
 * @ingroup gssapi
 */

GSSAPI_LIB_FUNCTION OM_uint32 GSSAPI_LIB_CALL
gss_import_name(OM_uint32 *minor_status,
    const gss_buffer_t input_name_buffer,
    const gss_OID input_name_type,
    gss_name_t *output_name)
{
        struct _gss_mechanism_name *mn;
	gss_OID			name_type = input_name_type;
	OM_uint32		major_status, ms;
	struct _gss_name	*name;
        struct _gss_mech_switch	*m;
	gss_name_t		rname;

	if (input_name_buffer == GSS_C_NO_BUFFER)
		return GSS_S_CALL_INACCESSIBLE_READ;
	if (output_name == NULL)
		return GSS_S_CALL_INACCESSIBLE_WRITE;

	*output_name = GSS_C_NO_NAME;

	/* Allow empty names since that's valid (ANONYMOUS for example) */

	_gss_load_mech();

	/*
	 * If this is an exported name, we need to parse it to find
	 * the mechanism and then import it as an MN. See RFC 2743
	 * section 3.2 for a description of the format.
	 */
	if (gss_oid_equal(name_type, GSS_C_NT_EXPORT_NAME) ||
            gss_oid_equal(name_type, GSS_C_NT_COMPOSITE_EXPORT)) {
                return _gss_import_export_name(minor_status, input_name_buffer,
                                               name_type, output_name);
	}


	*minor_status = 0;
	name = _gss_create_name(NULL, NULL);
	if (!name) {
		*minor_status = ENOMEM;
		return (GSS_S_FAILURE);
	}

	if (name_type != GSS_C_NO_OID) {
		major_status = _gss_intern_oid(minor_status,
					       name_type, &name->gn_type);
		if (major_status) {
			rname = (gss_name_t)name;
			gss_release_name(&ms, (gss_name_t *)&rname);
			return (GSS_S_FAILURE);
		}
	} else
		name->gn_type = GSS_C_NO_OID;

	major_status = _gss_copy_buffer(minor_status,
	    input_name_buffer, &name->gn_value);
	if (major_status)
		goto out;

	/*
	 * Walk over the mechs and import the name into a mech name
	 * for those supported this nametype.
	 */

	HEIM_TAILQ_FOREACH(m, &_gss_mechs, gm_link) {
		int present = 0;

                if ((m->gm_mech.gm_flags & GM_USE_MG_NAME))
                    continue;

		if (name_type != GSS_C_NO_OID) {
			    major_status = gss_test_oid_set_member(minor_status,
				    name_type, m->gm_name_types, &present);

			    if (GSS_ERROR(major_status) || present == 0)
					continue;
		}

		mn = malloc(sizeof(struct _gss_mechanism_name));
		if (!mn) {
			*minor_status = ENOMEM;
			major_status = GSS_S_FAILURE;
			goto out;
		}

		major_status = (*m->gm_mech.gm_import_name)(minor_status,
		    &name->gn_value,
		    name->gn_type,
		    &mn->gmn_name);
		if (major_status != GSS_S_COMPLETE) {
			_gss_mg_error(&m->gm_mech, *minor_status);
			free(mn);
			/**
			 * If we failed to import the name in a mechanism, it
			 * will be ignored as long as its possible to import
			 * name in some other mechanism. We will catch the
			 * failure later though in gss_init_sec_context() or
			 * another function.
			 */
			continue;
		}

		mn->gmn_mech = &m->gm_mech;
		mn->gmn_mech_oid = m->gm_mech_oid;
		HEIM_TAILQ_INSERT_TAIL(&name->gn_mn, mn, gmn_link);
	}

	/*
	 * If we can't find a mn for the name, bail out already here.
	 */

	mn = HEIM_TAILQ_FIRST(&name->gn_mn);
	if (!mn) {
		*minor_status = 0;
		major_status = GSS_S_NAME_NOT_MN;
		goto out;
	}

	*output_name = (gss_name_t) name;
	return (GSS_S_COMPLETE);

 out:
	rname = (gss_name_t)name;
	gss_release_name(&ms, &rname);
	return major_status;
}
