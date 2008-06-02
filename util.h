/*
 * Copyright © 2008 Jelmer Vernooij <jelmer@samba.org>
 * -*- coding: utf-8 -*-
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#ifndef _BZR_SVN_UTIL_H_
#define _BZR_SVN_UTIL_H_

apr_pool_t *Pool(apr_pool_t *parent);
bool check_error(svn_error_t *error);
apr_array_header_t *string_list_to_apr_array(apr_pool_t *pool, PyObject *l);
PyObject *prop_hash_to_dict(apr_hash_t *props);
svn_error_t *py_svn_log_wrapper(void *baton, apr_hash_t *changed_paths, 
								long revision, const char *author, 
								const char *date, const char *message, 
								apr_pool_t *pool);
svn_error_t *py_svn_error(void);
void PyErr_SetSubversionException(svn_error_t *error);

#define RUN_SVN_WITH_POOL(pool, cmd)  \
	if (!check_error((cmd))) { \
		apr_pool_destroy(pool); \
		return NULL; \
	}

PyObject *wrap_lock(svn_lock_t *lock);
apr_array_header_t *revnum_list_to_apr_array(apr_pool_t *pool, PyObject *l);
svn_stream_t *new_py_stream(apr_pool_t *pool, PyObject *py);
PyObject *PyErr_NewSubversionException(svn_error_t *error);

#endif /* _BZR_SVN_UTIL_H_ */
