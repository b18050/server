/*****************************************************************************

Copyright (c) 2006, 2016, Oracle and/or its affiliates. All Rights Reserved.
Copyright (c) 2019, MariaDB Corporation.

This program is free software; you can redistribute it and/or modify it under
the terms of the GNU General Public License as published by the Free Software
Foundation; version 2 of the License.

This program is distributed in the hope that it will be useful, but WITHOUT
ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License along with
this program; if not, write to the Free Software Foundation, Inc.,
51 Franklin Street, Fifth Floor, Boston, MA 02110-1335 USA

*****************************************************************************/

/**************************************************//**
@file row/row0ext.cc
Caching of externally stored column prefixes

Created September 2006 Marko Makela
*******************************************************/

#include "row0ext.h"
#include "btr0cur.h"

/** Fills the column prefix cache of an externally stored column.
@param[in,out]	ext		column prefix cache
@param[in]	i		index of ext->ext[]
@param[in]	space		tablespace
@param[in]	dfield		data field */
static
void
row_ext_cache_fill(
	row_ext_t*		ext,
	ulint			i,
	fil_space_t*		space,
	const dfield_t*		dfield)
{
	const byte*	field	= static_cast<const byte*>(
					dfield_get_data(dfield));
	ulint		f_len	= dfield_get_len(dfield);
	byte*		buf	= ext->buf + i * ext->max_len;

	ut_ad(ext->max_len > 0);
	ut_ad(i < ext->n_ext);
	ut_ad(dfield_is_ext(dfield));
	ut_a(f_len >= BTR_EXTERN_FIELD_REF_SIZE);

	if (UNIV_UNLIKELY(!memcmp(field_ref_zero,
				  field + f_len - BTR_EXTERN_FIELD_REF_SIZE,
				  BTR_EXTERN_FIELD_REF_SIZE))) {
		/* The BLOB pointer is not set: we cannot fetch it */
		ext->len[i] = 0;
	} else {
		if (ext->max_len == REC_VERSION_56_MAX_INDEX_COL_LEN
		    && f_len > BTR_EXTERN_FIELD_REF_SIZE) {
			/* In this case, the field is in B format or beyond,
			(refer to the definition of row_ext_t.max_len)
			and the field is already fill with prefix, otherwise
			f_len would be BTR_EXTERN_FIELD_REF_SIZE.
			So there is no need to re-read the prefix externally,
			but just copy the local prefix to buf. Please note
			if the ext->len[i] is zero, it means an error
			as above. */
			memcpy(buf, field, f_len - BTR_EXTERN_FIELD_REF_SIZE);
			ext->len[i] = f_len - BTR_EXTERN_FIELD_REF_SIZE;
		} else {
			/* Fetch at most ext->max_len of the column.
			The column should be non-empty.  However,
			trx_rollback_all_recovered() may try to
			access a half-deleted BLOB if the server previously
			crashed during the execution of
			btr_free_externally_stored_field(). */
			ext->len[i] = btr_copy_externally_stored_field_prefix(
				buf, ext->max_len, ext->zip_size,
				field, f_len);
		}
	}
}

/********************************************************************//**
Creates a cache of column prefixes of externally stored columns.
@return own: column prefix cache */
row_ext_t*
row_ext_create(
/*===========*/
	ulint		n_ext,	/*!< in: number of externally stored columns */
	const ulint*	ext,	/*!< in: col_no's of externally stored columns
				in the InnoDB table object, as reported by
				dict_col_get_no(); NOT relative to the records
				in the clustered index */
	const dict_table_t& table,	/*!< in: table */
	const dtuple_t*	tuple,	/*!< in: data tuple containing the field
				references of the externally stored
				columns; must be indexed by col_no;
				the clustered index record must be
				covered by a lock or a page latch
				to prevent deletion (rollback or purge). */
	mem_heap_t*	heap)	/*!< in: heap where created */
{
	if (!table.space) {
		return NULL;
	}

	ut_ad(n_ext > 0);

	row_ext_t* ret = static_cast<row_ext_t*>(
		mem_heap_alloc(heap,
			       (sizeof *ret) + (n_ext - 1) * sizeof ret->len));

	ret->n_ext = n_ext;
	ret->ext = ext;
	ret->max_len = DICT_MAX_FIELD_LEN_BY_FORMAT_FLAG(table.flags);
	ret->zip_size = dict_tf_get_zip_size(table.flags);

	ret->buf = static_cast<byte*>(
		mem_heap_alloc(heap, n_ext * ret->max_len));

	/* Fetch the BLOB prefixes */
	for (ulint i = 0; i < n_ext; i++) {
		const dfield_t*	dfield;

		dfield = dtuple_get_nth_field(tuple, ext[i]);
		row_ext_cache_fill(ret, i, table.space, dfield);
	}

	return(ret);
}
