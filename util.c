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
#include <stdbool.h>
#include <Python.h>
#include <apr_general.h>
#include <svn_error.h>
#include <svn_io.h>

apr_pool_t *Pool(apr_pool_t *parent)
{
    apr_status_t status;
    apr_pool_t *ret;
    char errmsg[1024];
    ret = NULL;
    status = apr_pool_create(&ret, parent);
    if (status != 0) {
        PyErr_SetString(PyExc_Exception, 
						apr_strerror(status, errmsg, sizeof(errmsg)));
		return NULL;
	}
    return ret;
}

PyObject *PyErr_NewSubversionException(svn_error_t *error)
{
	return NULL; /* FIXME */
}

void PyErr_SetSubversionException(svn_error_t *error)
{
	/* FIXME */
}

bool check_error(svn_error_t *error)
{
    if (error != NULL) {
		PyErr_SetSubversionException(error);
   		return false;
	}
	return true;
}

apr_array_header_t *string_list_to_apr_array(apr_pool_t *pool, PyObject *l)
{
    apr_array_header_t *ret;
	int i;
    if (l == Py_None) {
        return NULL;
	}
    ret = apr_array_make(pool, PyList_Size(l), 4);
	for (i = 0; i < PyList_Size(l); i++) {
		char **el = (char **)apr_array_push(ret);
        *el = PyString_AsString(PyList_GetItem(l, i));
	}
    return ret;
}

PyObject *prop_hash_to_dict(apr_hash_t *props)
{
    const char *key;
    apr_hash_index_t *idx;
    apr_ssize_t klen;
    svn_string_t *val;
    apr_pool_t *pool;
	PyObject *py_props;
    if (props == NULL) {
        return Py_None;
	}
    pool = Pool(NULL);
    py_props = PyDict_New();
    for (idx = apr_hash_first(pool, props); idx != NULL; 
		 idx = apr_hash_next(idx)) {
        apr_hash_this(idx, (const void **)&key, &klen, (void **)&val);
        PyDict_SetItemString(py_props, key, 
							 PyString_FromStringAndSize(val->data, val->len));
	}
    apr_pool_destroy(pool);
    return py_props;
}

svn_error_t *py_svn_log_wrapper(void *baton, apr_hash_t *changed_paths, long revision, const char *author, const char *date, const char *message, apr_pool_t *pool)
{
    apr_hash_index_t *idx;
    const char *key;
    apr_ssize_t klen;
    svn_log_changed_path_t *val;
	PyObject *revprops, *py_changed_paths, *ret;

    if (changed_paths == NULL) {
        py_changed_paths = Py_None;
	} else {
        py_changed_paths = PyDict_New();
        for (idx = apr_hash_first(pool, changed_paths); idx != NULL;
             idx = apr_hash_next(idx)) {
            apr_hash_this(idx, (const void **)&key, &klen, (void **)&val);
			PyDict_SetItemString(py_changed_paths, key, 
					Py_BuildValue("(czi)", val->action, val->copyfrom_path, 
                                         val->copyfrom_rev));
		}
	}
    revprops = PyDict_New();
    if (message != NULL) {
        PyDict_SetItemString(revprops, SVN_PROP_REVISION_LOG, 
							 PyString_FromString(message));
	}
    if (author != NULL) {
        PyDict_SetItemString(revprops, SVN_PROP_REVISION_AUTHOR, 
							 PyString_FromString(author));
	}
    if (date != NULL) {
        PyDict_SetItemString(revprops, SVN_PROP_REVISION_DATE, 
							 PyString_FromString(date));
	}
    ret = PyObject_CallFunction((PyObject *)baton, "OiO", py_changed_paths, 
								 revision, revprops);
	/* FIXME: Handle ret != NULL */
	return NULL;
}

svn_error_t *py_svn_error(void)
{
	return NULL; /* FIXME */
}

PyObject *wrap_lock(svn_lock_t *lock)
{
    return Py_BuildValue("(zzzbzz)", lock->path, lock->token, lock->owner, 
						 lock->comment, lock->is_dav_comment, 
						 lock->creation_date, lock->expiration_date);
}

apr_array_header_t *revnum_list_to_apr_array(apr_pool_t *pool, PyObject *l)
{
	int i;
    apr_array_header_t *ret;
    if (l == Py_None) {
        return NULL;
	}
    ret = apr_array_make(pool, PyList_Size(l), sizeof(svn_revnum_t));
	if (ret == NULL) {
		PyErr_NoMemory();
		return NULL;
	}
    for (i = 0; i < PyList_Size(l); i++) {
		svn_revnum_t *el = (svn_revnum_t *)apr_array_push(ret);
        *el = PyLong_AsLong(PyList_GetItem(l, i));
	}
    return ret;
}


static svn_error_t *py_stream_read(void *baton, char *buffer, apr_size_t *length)
{
    PyObject *self = (PyObject *)baton, *ret;
    ret = PyObject_CallMethod(self, "read", "i", *length);
	if (ret == NULL)
		return py_svn_error();
	/* FIXME: Handle !PyString_Check(ret) */
    *length = PyString_Size(ret);
    memcpy(buffer, PyString_AS_STRING(ret), *length);
    return NULL;
}

static svn_error_t *py_stream_write(void *baton, const char *data, apr_size_t *len)
{
    PyObject *self = (PyObject *)baton, *ret;
    ret = PyObject_CallMethod(self, "write", "s#", data, len[0]);
	if (ret == NULL)
		return py_svn_error();
	return NULL;
}

static svn_error_t *py_stream_close(void *baton)
{
    PyObject *self = (PyObject *)baton, *ret;
    ret = PyObject_CallMethod(self, "close", NULL);
	if (ret == NULL)
		return py_svn_error();
    Py_DECREF(self);
	return NULL;
}

svn_stream_t *string_stream(apr_pool_t *pool, PyObject *text)
{
    svn_stringbuf_t *buf;
    buf = svn_stringbuf_ncreate(PyString_AsString(text), 
								PyString_Size(text), pool);
    return svn_stream_from_stringbuf(buf, pool);
}

svn_stream_t *new_py_stream(apr_pool_t *pool, PyObject *py)
{
    svn_stream_t *stream;
    Py_INCREF(py);
    stream = svn_stream_create((void *)py, pool);
    svn_stream_set_read(stream, py_stream_read);
    svn_stream_set_write(stream, py_stream_write);
    svn_stream_set_close(stream, py_stream_close);
    return stream;
}
