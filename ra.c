/* Copyright © 2008 Jelmer Vernooij <jelmer@samba.org>
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
#include <svn_types.h>
#include <svn_ra.h>

#include "util.h"

PyAPI_DATA(PyTypeObject) DirectoryEditor_Type;
PyAPI_DATA(PyTypeObject) FileEditor_Type;
PyAPI_DATA(PyTypeObject) Editor_Type;
PyAPI_DATA(PyTypeObject) Reporter_Type;
PyAPI_DATA(PyTypeObject) RemoteAccess_Type;
PyAPI_DATA(PyTypeObject) Auth_Type;
PyAPI_DATA(PyTypeObject) AuthProvider_Type;

typedef struct {
	PyObject_HEAD
    svn_auth_baton_t *auth_baton;
    apr_pool_t *pool;
    PyObject *providers;
} AuthObject;

svn_error_t *py_commit_callback(const svn_commit_info_t *commit_info, void *baton, apr_pool_t *pool)
{
	PyObject *fn = (PyObject *)baton, *ret;

	ret = PyObject_CallFunction(fn, "izz", 
								commit_info->revision, commit_info->date, 
								commit_info->author);
	if (ret == NULL)
		return py_svn_error();
	return NULL;
}

PyObject *pyify_lock(svn_lock_t *lock)
{
    return Py_None; /* FIXME */
}

svn_error_t *py_lock_func (void *baton, const char *path, int do_lock, 
                           svn_lock_t *lock, svn_error_t *ra_err, 
                           apr_pool_t *pool)
{
    PyObject *py_ra_err = Py_None, *ret;
    if (ra_err != NULL) {
        py_ra_err = SubversionException(ra_err.apr_err, ra_err.message);
	}
    ret = PyObject_CallFunction((PyObject *)baton, "zbOO", path, do_lock, 
						  pyify_lock(lock), py_ra_err);
	if (ret == NULL)
		return py_svn_error();
	return NULL;
}

void py_progress_func(apr_off_t progress, apr_off_t total, void *baton, apr_pool_t *pool)
{
    PyObject *fn = (PyObject *)baton;
    if (fn == Py_None) {
        return;
	}
	PyObject_CallFunction(fn, "ll", progress, total);
	/* TODO: What to do with exceptions raised here ? */
}

char *c_lock_token(PyObject *py_lock_token)
{
    if (py_lock_token == Py_None) {
        return NULL;
    } else {
        return PyString_AsString(py_lock_token);
	}
}

typedef struct {
	PyObject_HEAD
    const svn_ra_reporter2_t *reporter;
    void *report_baton;
    apr_pool_t *pool;
} ReporterObject;

static PyObject *reporter_set_path(PyObject *self, PyObject *args)
{
	char *path; 
	svn_revnum_t revision; 
	bool start_empty; 
	PyObject *lock_token = Py_None;
	ReporterObject *reporter = (ReporterObject *)self;

	if (!PyArg_ParseTuple(args, "slb|z", &path, &revision, &start_empty, 
						  &lock_token))
		return NULL;

    if (!check_error(reporter->reporter->set_path(reporter->report_baton, 
												  path, revision, start_empty, 
					 c_lock_token(lock_token), reporter->pool)))
		return NULL;

	return Py_None;
}

static PyObject *reporter_delete_path(PyObject *self, PyObject *args)
{
	ReporterObject *reporter = (ReporterObject *)self;
	char *path;
	if (!PyArg_ParseTuple(args, "s", &path))
		return NULL;

	if (!check_error(reporter->reporter->delete_path(reporter->report_baton, 
													path, reporter->pool)))
		return NULL;

	return Py_None;
}

static PyObject *reporter_link_path(PyObject *self, PyObject *args)
{
	char *path, *url;
	svn_revnum_t revision;
	bool start_empty;
	PyObject *lock_token = Py_None;
	ReporterObject *reporter = (ReporterObject *)self;

	if (!PyArg_ParseTuple(args, "sslb|O", &path, &url, &start_empty, &lock_token))
		return NULL;

	if (!check_error(reporter->reporter->link_path(reporter->report_baton, path, url, 
				revision, start_empty, c_lock_token(lock_token), reporter->pool)))
		return NULL;

	return Py_None;
}

static PyObject *reporter_finish(PyObject *self)
{
	ReporterObject *reporter = (ReporterObject *)self;
	if (!check_error(reporter->reporter->finish_report(reporter->report_baton, 
													  reporter->pool)))
		return NULL;

	return Py_None;
}

static PyObject *reporter_abort(PyObject *self)
{
	ReporterObject *reporter = (ReporterObject *)self;
	if (!check_error(reporter->reporter->abort_report(reporter->report_baton, 
													 reporter->pool)))
		return NULL;

	return Py_None;
}

static PyMethodDef reporter_methods[] = {
	{ "abort", (PyCFunction)reporter_abort, METH_NOARGS, NULL },
	{ "finish", (PyCFunction)reporter_finish, METH_NOARGS, NULL },
	{ "link_path", (PyCFunction)reporter_link_path, METH_VARARGS, NULL },
	{ "set_path", (PyCFunction)reporter_set_path, METH_VARARGS, NULL },
	{ "delete_path", (PyCFunction)reporter_delete_path, METH_VARARGS, NULL },
	{ NULL }
};

static void reporter_dealloc(PyObject *self)
{
	ReporterObject *reporter = (ReporterObject *)self;
	/* FIXME: Warn the user if abort_report/finish_report wasn't called? */
	apr_pool_destroy(reporter->pool);
}

typedef struct {
	PyObject_HEAD
    const svn_delta_editor_t *editor;
    void *baton;
    apr_pool_t *pool;
} EditorObject;

static PyObject *new_editor_object(const svn_delta_editor_t *editor, void *baton, apr_pool_t *pool, PyTypeObject *type)
{
	EditorObject *obj = PyObject_New(EditorObject, type);
	if (obj == NULL)
		return NULL;
	obj->editor = editor;
    obj->baton = baton;
	obj->pool = pool;
	return (PyObject *)obj;
}

static PyObject *py_file_editor_apply_textdelta(PyObject *self, PyObject *args)
{
	EditorObject *editor = (EditorObject *)self;
	char *c_base_checksum = NULL;
	svn_txdelta_window_handler_t txdelta_handler;
	void *txdelta_baton;
	if (!PyArg_ParseTuple(args, "|z", &c_base_checksum))
		return NULL;
	if (!check_error(editor->editor->apply_textdelta(editor->baton,
				c_base_checksum, editor->pool, 
				&txdelta_handler, &txdelta_baton)))
		return NULL;
	py_txdelta = TxDeltaWindowHandler()
	py_txdelta.txdelta = txdelta_handler;
	py_txdelta.txbaton = txdelta_baton;
	return py_txdelta;
}

static PyObject *py_file_editor_change_prop(PyObject *self, PyObject *args)
{
	EditorObject *editor = (EditorObject *)self;
	char *name;
   	svn_string_t c_value;
	if (!PyArg_ParseTuple(args, "sz#", &name, &c_value.data, &c_value.len))
		return NULL;
	if (!check_error(editor->editor->change_file_prop(editor->baton, name, 
				&c_value, editor->pool)))
		return NULL;
	return Py_None;
}

static PyObject *py_file_editor_close(PyObject *self, PyObject *args)
{
	EditorObject *editor = (EditorObject *)self;
	char *c_checksum = NULL;
	if (!PyArg_ParseTuple(args, "|z", &c_checksum))
		return NULL;
	if (!check_error(editor->editor->close_file(editor->baton, c_checksum, 
                    editor->pool)))
		return NULL;
	return Py_None;
}

static PyMethodDef py_file_editor_methods[] = {
	{ "change_prop", py_file_editor_change_prop, METH_VARARGS, NULL },
	{ "close", py_file_editor_close, METH_VARARGS, NULL },
	{ "apply_textdelta", py_file_editor_apply_textdelta, METH_VARARGS, NULL },
	{ NULL }
};

static PyObject *py_dir_editor_delete_entry(PyObject *self, PyObject *args)
{
	EditorObject *editor = (EditorObject *)self;
	char *path; 
	svn_revnum_t revision = -1;

	if (!PyArg_ParseTuple(args, "s|l", &path, &revision))
		return NULL;

	if (!check_error(editor->editor->delete_entry(path, revision, editor->baton,
                                             editor->pool)))
		return NULL;

	return Py_None;
}

static PyObject *py_dir_editor_add_directory(PyObject *self, PyObject *args)
{
	char *path;
	char *copyfrom_path=NULL; 
	int copyfrom_rev=-1;
   	void *child_baton;
	EditorObject *editor = (EditorObject *)self;

	if (!PyArg_ParseTuple(args, "s|zl", &path, &copyfrom_path, &copyfrom_rev))
		return NULL;

	if (!check_error(editor->editor->add_directory(path, editor->baton,
                    copyfrom_path, copyfrom_rev, editor->pool, &child_baton)))
		return NULL;

    return new_editor_object(editor->editor, child_baton, editor->pool, 
							 &DirectoryEditor_Type);
}

static PyObject *py_dir_editor_open_directory(PyObject *self, PyObject *args)
{
	char *path;
	EditorObject *editor = (EditorObject *)self;
	int base_revision=-1;
	void *child_baton;
	if (!PyArg_ParseTuple(args, "s|l", &path, &base_revision))
		return NULL;

	if (!check_error(editor->editor->open_directory(path, editor->baton,
                    base_revision, editor->pool, &child_baton)))
		return NULL;

    return new_editor_object(editor->editor, child_baton, editor->pool, 
							 &DirectoryEditor_Type);
}

static PyObject *py_dir_editor_change_prop(PyObject *self, PyObject *args)
{
	char *name;
	svn_string_t c_value, *p_c_value;
	EditorObject *editor = (EditorObject *)self;

	if (!PyArg_ParseTuple(args, "sz#", &name, &c_value.data, &c_value.len))
		return NULL;

	if (!check_error(editor->editor->change_dir_prop(editor->baton, name, 
                    p_c_value, editor->pool)))
		return NULL;

	return Py_None;
}

static PyObject *py_dir_editor_close(PyObject *self)
{
	EditorObject *editor = (EditorObject *)self;
    if (!check_error(editor->editor->close_directory(editor->baton, 
													 editor->pool)))
		return NULL;

	return Py_None;
}

static PyObject *py_dir_editor_absent_directory(PyObject *self, PyObject *args)
{
	char *path;
	EditorObject *editor = (EditorObject *)self;

	if (!PyArg_ParseTuple(args, "s", &path))
		return NULL;
    
	if (!check_error(editor->editor->absent_directory(path, editor->baton, 
                    editor->pool)))
		return NULL;

	return Py_None;
}

static PyObject *py_dir_editor_add_file(PyObject *self, PyObject *args)
{
	char *path, *copy_path=NULL;
	int copy_rev=-1;
	void *file_baton;
	EditorObject *editor = (EditorObject *)self;

	if (!PyArg_ParseTuple(args, "s|zl", &path, &copy_path, &copy_rev))
		return NULL;

	if (!check_error(editor->editor->add_file(path, editor->baton, copy_path,
                    copy_rev, editor->pool, &file_baton)))
		return NULL;

	return new_editor_object(editor->editor, file_baton, editor->pool,
							 &FileEditor_Type);
}

static PyObject *py_dir_editor_open_file(PyObject *self, PyObject *args)
{
	char *path;
	int base_revision=-1;
	void *file_baton;
	EditorObject *editor = (EditorObject *)self;

	if (!PyArg_ParseTuple(args, "s|l", &path, &base_revision))
		return NULL;

	if (!check_error(editor->editor->open_file(path, editor->baton, 
                    base_revision, editor->pool, &file_baton)))
		return NULL;

	return new_editor_object(editor->editor, file_baton, editor->pool,
							 &FileEditor_Type);
}

static PyObject *py_dir_editor_absent_file(PyObject *self, PyObject *args)
{
	char *path;
	EditorObject *editor = (EditorObject *)self;
	if (!PyArg_ParseTuple(args, "s", &path))
		return NULL;

	if (!check_error(editor->editor->absent_file(path, editor->baton, editor->pool)))
		return NULL;

	return Py_None;
}

static PyMethodDef py_dir_editor_methods[] = {
	{ "absent_file", py_dir_editor_absent_file, METH_VARARGS, NULL },
	{ "absent_directory", py_dir_editor_absent_directory, METH_VARARGS, NULL },
	{ "delete_entry", py_dir_editor_delete_entry, METH_VARARGS, NULL },
	{ "add_file", py_dir_editor_add_file, METH_VARARGS, NULL },
	{ "open_file", py_dir_editor_open_file, METH_VARARGS, NULL },
	{ "add_directory", py_dir_editor_add_directory, METH_VARARGS, NULL },
	{ "open_directory", py_dir_editor_open_directory, METH_VARARGS, NULL },
	{ "close", (PyCFunction)py_dir_editor_close, METH_NOARGS, NULL },
	{ "change_prop", py_dir_editor_change_prop, METH_VARARGS, NULL },

	{ NULL }
};

static PyObject *py_editor_set_target_revision(PyObject *self, PyObject *args)
{
	int target_revision;
	EditorObject *editor = (EditorObject *)self;
	if (!PyArg_ParseTuple(args, "i", &target_revision))
		return NULL;

	if (!check_error(editor->editor->set_target_revision(editor->baton,
                    target_revision, editor->pool)))
		return NULL;

	return Py_None;
}
    
static PyObject *py_editor_open_root(PyObject *self, PyObject *args)
{
	int base_revision=-1;
	void *root_baton;
	EditorObject *editor = (EditorObject *)self;

	if (!PyArg_ParseTuple(args, "|i", &base_revision))
		return NULL;

    if (!check_error(editor->editor->open_root(editor->baton, base_revision,
                    editor->pool, &root_baton)))
		return NULL;

	return new_editor_object(editor->editor, root_baton, editor->pool,
							 &DirectoryEditor_Type);
}

static PyObject *py_editor_close(PyObject *self)
{
	EditorObject *editor = (EditorObject *)self;
	if (!check_error(editor->editor->close_edit(editor->baton, editor->pool)))
		return NULL;

	return Py_None;
}

static PyObject *py_editor_abort(PyObject *self)
{
	EditorObject *editor = (EditorObject *)self;

	if (!check_error(editor->editor->abort_edit(editor->baton, editor->pool)))
		return NULL;

	return Py_None;
}

static PyMethodDef py_editor_methods[] = { 
	{ "abort", (PyCFunction)py_editor_abort, METH_NOARGS, NULL },
	{ "close", (PyCFunction)py_editor_close, METH_NOARGS, NULL },
	{ "open_root", py_editor_open_root, METH_VARARGS, NULL },
	{ "set_target_revision", py_editor_set_target_revision, METH_VARARGS, NULL },
	{ NULL }
};

static void py_editor_dealloc(PyObject *self)
{
	EditorObject *editor = (EditorObject *)self;
	apr_pool_destroy(editor->pool);
}

/**
 * Get libsvn_ra version information.
 *
 * :return: tuple with major, minor, patch version number and tag.
 */
PyObject *version(PyObject *self)
{
    const svn_version_t *ver = svn_ra_version();
    return Py_BuildValue("(iiii)", ver->major, ver->minor, 
						 ver->patch, ver->tag);
}

static svn_error_t *py_cb_editor_set_target_revision(void *edit_baton, svn_revnum_t target_revision, apr_pool_t *pool)
{
    PyObject *self = (PyObject *)edit_baton, *ret;

	ret = PyObject_CallMethod(self, "set_target_revision", "l", target_revision);
	if (ret == NULL)
		return py_svn_error();
    return NULL;
}

static svn_error_t *py_cb_editor_open_root(void *edit_baton, svn_revnum_t base_revision, apr_pool_t *dir_pool, void **root_baton)
{
    PyObject *self = (PyObject *)edit_baton, *ret;
    *root_baton = NULL;
    ret = PyObject_CallMethod(self, "open_root", "l", base_revision);
	if (ret == NULL)
		return py_svn_error();
    Py_INCREF(ret);
    *root_baton = (void *)ret;
    return NULL;
}

static svn_error_t *py_cb_editor_delete_entry(const char *path, long revision, void *parent_baton, apr_pool_t *pool)
{
    PyObject *self = (PyObject *)parent_baton, *ret;
	ret = PyObject_CallMethod(self, "delete_entry", "sl", path, revision);
	if (ret == NULL)
		return py_svn_error();
    return NULL;
}

static svn_error_t *py_cb_editor_add_directory(const char *path, void *parent_baton, const char *copyfrom_path, long copyfrom_revision, apr_pool_t *dir_pool, void **child_baton)
{
    PyObject *self = (PyObject *)parent_baton, *ret;
    *child_baton = NULL;
    if (copyfrom_path == NULL) {
        ret = PyObject_CallMethod(self, "add_directory", "s", path);
	} else {
        ret = PyObject_CallMethod(self, "add_directory", "ssl", path, copyfrom_path, copyfrom_revision);
	}
	if (ret == NULL)
		return py_svn_error();
    Py_INCREF(ret);
    *child_baton = (void *)ret;
    return NULL;
}

svn_error_t *py_cb_editor_open_directory(const char *path, void *parent_baton, long base_revision, apr_pool_t *dir_pool, void **child_baton)
{
    PyObject *self = (PyObject *)parent_baton, *ret;
    *child_baton = NULL;
    ret = PyObject_CallMethod(self, "open_directory", "sl", path, base_revision);
	if (ret == NULL)
		return py_svn_error();
    Py_INCREF(ret);
    *child_baton = (void *)ret;
    return NULL;
}

static svn_error_t *py_cb_editor_change_dir_prop(void *dir_baton, const char *name, const svn_string_t *value, apr_pool_t *pool)
{
    PyObject *self = (PyObject *)dir_baton, *ret;
	ret = PyObject_CallMethod(self, "change_prop", "sz#", name, value->data, value->len);
	if (ret == NULL)
		return py_svn_error();
    return NULL;
}

static svn_error_t *py_cb_editor_close_directory(void *dir_baton, apr_pool_t *pool)
{
    PyObject *self = (PyObject *)dir_baton, *ret;
    ret = PyObject_CallMethod(self, "close", NULL);
	if (ret == NULL)
		return py_svn_error();
    Py_DECREF(self);
    return NULL;
}

static svn_error_t *py_cb_editor_absent_directory(const char *path, void *parent_baton, apr_pool_t *pool)
{
    PyObject *self = (PyObject *)parent_baton, *ret;
	ret = PyObject_CallMethod(self, "absent_directory", "s", path);
	if (ret == NULL)
		return py_svn_error();
    return NULL;
}

static svn_error_t *py_cb_editor_add_file(const char *path, void *parent_baton, const char *copy_path, long copy_revision, apr_pool_t *file_pool, void **file_baton)
{
    PyObject *self = (PyObject *)parent_baton, *ret;
    if (copy_path == NULL) {
		ret = PyObject_CallMethod(self, "add_file", "s", path);
	} else {
		ret = PyObject_CallMethod(self, "add_file", "ssl", path, copy_path, 
								  copy_revision);
	}
	if (ret == NULL)
		return py_svn_error();
    Py_INCREF(ret);
    *file_baton = (void *)ret;
    return NULL;
}

static svn_error_t *py_cb_editor_open_file(const char *path, void *parent_baton, long base_revision, apr_pool_t *file_pool, void **file_baton)
{
    PyObject *self = (PyObject *)parent_baton, *ret;
    ret = PyObject_CallMethod(self, "open_file", "sl", path, base_revision);
	if (ret == NULL)
		return py_svn_error();
    Py_INCREF(ret);
    *file_baton = (void *)ret;
    return NULL;
}

static svn_error_t *py_txdelta_window_handler(svn_txdelta_window_t *window, void *baton)
{
	int i;
	PyObject *ops, *ret;
    PyObject *fn = (PyObject *)baton;
    if (window == NULL) {
        /* Signals all delta windows have been received */
        Py_DECREF(fn);
        return NULL;
	}
    if (fn == Py_None) {
        /* User doesn't care about deltas */
        return NULL;
	}
    ops = PyList_New(window->num_ops);
	for (i = 0; i < window->num_ops; i++) {
		PyList_SetItem(ops, i, Py_BuildValue("(ill)", window->ops[i].action_code, 
					window->ops[i].offset, 
					window->ops[i].length));
	}
	ret = PyObject_CallFunction(fn, "(llllOs#)", 
								window->sview_offset, 
								window->sview_len, 
								window->tview_len, 
								window->src_ops, ops, 
								window->new_data->data, window->new_data->len);
	if (ret == NULL)
		return py_svn_error();
    return NULL;
}

static svn_error_t *py_cb_editor_apply_textdelta(void *file_baton, const char *base_checksum, apr_pool_t *pool, svn_txdelta_window_handler_t *handler, void **handler_baton)
{
    PyObject *self = (PyObject *)file_baton, *ret;
    *handler_baton = NULL;
	ret = PyObject_CallMethod(self, "apply_textdelta", "z", base_checksum);
	if (ret == NULL)
		return py_svn_error();
    Py_INCREF(ret);
    *handler_baton = (void *)ret;
    *handler = py_txdelta_window_handler;
    return NULL;
}

static svn_error_t *py_cb_editor_change_file_prop(void *file_baton, const char *name, const svn_string_t *value, apr_pool_t *pool)
{
    PyObject *self = (PyObject *)file_baton, *ret;
	ret = PyObject_CallMethod(self, "change_prop", "sz#", name, 
							  value->data, value->len);
	if (ret == NULL)
		return py_svn_error();
    return NULL;
}

static svn_error_t *py_cb_editor_close_file(void *file_baton, 
										 const char *text_checksum, apr_pool_t *pool)
{
    PyObject *self = (PyObject *)file_baton, *ret;
    if (text_checksum != NULL) {
		ret = PyObject_CallMethod(self, "close", NULL);
	} else {
		ret = PyObject_CallMethod(self, "close", "s", text_checksum);
	}
	if (ret == NULL)
		return py_svn_error();
    Py_DECREF(self);
    return NULL;
}

static svn_error_t *py_cb_editor_absent_file(const char *path, void *parent_baton, apr_pool_t *pool)
{
    PyObject *self = (PyObject *)parent_baton, *ret;
	ret = PyObject_CallMethod(self, "absent_file", "s", path);
	if (ret == NULL)
		return py_svn_error();
    return NULL;
}

static svn_error_t *py_cb_editor_close_edit(void *edit_baton, apr_pool_t *pool)
{
    PyObject *self = (PyObject *)edit_baton, *ret;
	ret = PyObject_CallMethod(self, "close", NULL);
	if (ret == NULL)
		return py_svn_error();
    return NULL;
}

static svn_error_t *py_cb_editor_abort_edit(void *edit_baton, apr_pool_t *pool)
{
    PyObject *self = (PyObject *)edit_baton, *ret;
	ret = PyObject_CallMethod(self, "abort", NULL);
	if (ret == NULL)
		return py_svn_error();
    return NULL;
}

svn_delta_editor_t py_editor = {
	.set_target_revision = py_cb_editor_set_target_revision,
	.open_root = py_cb_editor_open_root,
	.delete_entry = py_cb_editor_delete_entry,
	.add_directory = py_cb_editor_add_directory,
	.open_directory = py_cb_editor_open_directory,
	.change_dir_prop = py_cb_editor_change_dir_prop,
	.close_directory = py_cb_editor_close_directory,
	.absent_directory = py_cb_editor_absent_directory,
	.add_file = py_cb_editor_add_file,
	.open_file = py_cb_editor_open_file,
	.apply_textdelta = py_cb_editor_apply_textdelta,
	.change_file_prop = py_cb_editor_change_file_prop,
	.close_file = py_cb_editor_close_file,
	.absent_file = py_cb_editor_absent_file,
	.close_edit = py_cb_editor_close_edit,
	.abort_edit = py_cb_editor_abort_edit
};

static svn_error_t *py_file_rev_handler(void *baton, const char *path, svn_revnum_t rev, apr_hash_t *rev_props, svn_txdelta_window_handler_t *delta_handler, void **delta_baton, apr_array_header_t *prop_diffs, apr_pool_t *pool)
{
    PyObject *fn = (PyObject *)baton, *ret;

	ret = PyObject_CallFunction(fn, "slO", path, rev, 
								prop_hash_to_dict(rev_props));
	if (ret == NULL)
		return py_svn_error();
    return NULL;
}

/** Connection to a remote Subversion repository. */
typedef struct {
	svn_ra_session_t *ra;
    apr_pool_t *pool;
    PyObject *url;
    PyObject *progress_func;
	AuthObject *auth;
} RemoteAccessObject;

static PyObject *ra_new(PyTypeObject *type, PyObject *args, PyObject *kwargs)
{
	char *kwnames[] = { "url", "progress_cb", "auth", "config", NULL };
	PyObject *url;
	PyObject *progress_cb = Py_None;
	AuthObject *auth = (AuthObject *)Py_None;
	PyObject *config = Py_None;
	RemoteAccessObject *ret;
	apr_hash_t *config_hash;
	svn_ra_callbacks2_t *callbacks2;
	Py_ssize_t idx = 0;
	PyObject *key, *value;

	if (!PyArg_ParseTupleAndKeywords(args, kwargs, "O|OOO", kwnames, &url, &progress_cb, (PyObject **)&auth, &config))
		return NULL;

	ret = PyObject_New(RemoteAccessObject, &RemoteAccess_Type);
	if (ret == NULL)
		return NULL;

	if ((PyObject *)auth == Py_None) {
		auth = PyObject_New(AuthObject, &Auth_Type);
	} else {
		/* FIXME: check auth is an instance of Auth_Type */
		Py_INCREF(auth);
	}
	
	ret->url = url;
	Py_INCREF(url);
    ret->auth = auth;
    ret->pool = Pool(NULL);
	if (!check_error(svn_ra_create_callbacks(&callbacks2, ret->pool))) {
		apr_pool_destroy(ret->pool);
		PyObject_Del(ret);
		return NULL;
	}

	callbacks2->progress_func = py_progress_func;
	callbacks2->auth_baton = ret->auth->auth_baton;
	ret->progress_func = progress_cb;
	callbacks2->progress_baton = (void *)ret->progress_func;
	config_hash = apr_hash_make(ret->pool);
	while (PyDict_Next(config, &idx, &key, &value)) {
		apr_hash_set(config_hash, PyString_AsString(key), 
					 PyString_Size(key), PyString_AsString(value));
	}
	if (!check_error(svn_ra_open2(&ret->ra, PyString_AsString(url), 
								  callbacks2, NULL, config_hash, ret->pool))) {
		apr_pool_destroy(ret->pool);
		PyObject_Del(ret);
		return NULL;
	}
	return (PyObject *)ret;
}

 /**
  * Obtain the globally unique identifier for this repository.
  */
static PyObject *ra_get_uuid(PyObject *self)
{
	const char *uuid;
	RemoteAccessObject *ra = (RemoteAccessObject *)self;
	PyObject *ret;
    apr_pool_t *temp_pool;
    temp_pool = Pool(ra->pool);
	RUN_SVN_WITH_POOL(temp_pool, svn_ra_get_uuid(ra->ra, &uuid, temp_pool));
	ret = PyString_FromString(uuid);
	apr_pool_destroy(temp_pool);
	return ret;
}

/** Switch to a different url. */
static PyObject *ra_reparent(PyObject *self, PyObject *args)
{
	char *url;
	apr_pool_t *temp_pool;
	RemoteAccessObject *ra = (RemoteAccessObject *)self;

	if (!PyArg_ParseTuple(args, "s", &url))
		return NULL;

	temp_pool = Pool(ra->pool);
	RUN_SVN_WITH_POOL(temp_pool, svn_ra_reparent(ra->ra, url, temp_pool));
	apr_pool_destroy(temp_pool);
	return Py_None;
}

/**
 * Obtain the number of the latest committed revision in the 
 * connected repository.
 */
static PyObject *ra_get_latest_revnum(PyObject *self)
{
	RemoteAccessObject *ra = (RemoteAccessObject *)self;
	long latest_revnum;
    apr_pool_t *temp_pool;
    temp_pool = Pool(ra->pool);
	RUN_SVN_WITH_POOL(temp_pool, 
				  svn_ra_get_latest_revnum(ra->ra, &latest_revnum, temp_pool));
    apr_pool_destroy(temp_pool);
    return PyLong_FromLong(latest_revnum);
}

static PyObject *ra_get_log(PyObject *self, PyObject *args, PyObject *kwargs)
{
	char *kwnames[] = { "callback", "paths", "start", "end", "limit",
		"discover_changed_paths", "strict_node_history", "revprops", NULL };
	PyObject *callback, *paths;
	svn_revnum_t start, end;
	int limit=0; 
	bool discover_changed_paths=true, strict_node_history=true;
	RemoteAccessObject *ra = (RemoteAccessObject *)self;
	PyObject *revprops = Py_None;
    apr_pool_t *temp_pool;

	if (PyArg_ParseTupleAndKeywords(args, kwargs, "OOll|ibbO", kwnames, 
						 &callback, &paths, &start, &end, &limit,
						 &discover_changed_paths, &strict_node_history,
						 &revprops))
		return NULL;

    temp_pool = Pool(NULL);
	RUN_SVN_WITH_POOL(temp_pool, svn_ra_get_log(ra->ra, 
            string_list_to_apr_array(temp_pool, paths), start, end, limit,
            discover_changed_paths, strict_node_history, py_svn_log_wrapper, 
            callback, temp_pool));
    apr_pool_destroy(temp_pool);
	return Py_None;
}

/**
 * Obtain the URL of the root of this repository.
 */
static PyObject *ra_get_repos_root(PyObject *self)
{
	RemoteAccessObject *ra = (RemoteAccessObject *)self;
	const char *root;
    apr_pool_t *temp_pool = Pool(ra->pool);
	RUN_SVN_WITH_POOL(temp_pool, 
					  svn_ra_get_repos_root(ra->ra, &root, temp_pool));
	apr_pool_destroy(temp_pool);
	return PyString_FromString(root);
}

static PyObject *ra_do_update(PyObject *self, PyObject *args)
{
	svn_revnum_t revision_to_update_to;
	char *update_target; 
	bool recurse;
	PyObject *update_editor;
    const svn_ra_reporter2_t *reporter;
	void *report_baton;
	apr_pool_t *temp_pool;
	ReporterObject *ret;
	RemoteAccessObject *ra = (RemoteAccessObject *)self;

	if (!PyArg_ParseTuple(args, "lsbO", &revision_to_update_to, &update_target, &recurse, &update_editor))
		return NULL;

	temp_pool = Pool(ra->pool);
	RUN_SVN_WITH_POOL(temp_pool, svn_ra_do_update(ra->ra, &reporter, 
												  &report_baton, 
												  revision_to_update_to, 
												  update_target, recurse, 
												  &py_editor, update_editor, 
												  temp_pool));
	ret = PyObject_New(ReporterObject, &Reporter_Type);
	if (ret == NULL)
		return NULL;
	ret->reporter = reporter;
	ret->report_baton = report_baton;
	ret->pool = temp_pool;
	return (PyObject *)ret;
}

static PyObject *ra_do_switch(PyObject *self, PyObject *args)
{
	RemoteAccessObject *ra = (RemoteAccessObject *)self;
	svn_revnum_t revision_to_update_to;
	char *update_target; 
	bool recurse;
	char *switch_url; 
	PyObject *update_editor;
	const svn_ra_reporter2_t *reporter;
	void *report_baton;
	apr_pool_t *temp_pool;
	ReporterObject *ret;

	if (!PyArg_ParseTuple(args, "lsbsO", &revision_to_update_to, &update_target, 
						  &recurse, &switch_url, &update_editor))
		return NULL;
	temp_pool = Pool(ra->pool);
	RUN_SVN_WITH_POOL(temp_pool, svn_ra_do_switch(
						ra->ra, &reporter, &report_baton, 
						revision_to_update_to, update_target, 
						recurse, switch_url, &py_editor, 
						update_editor, temp_pool));
	ret = PyObject_New(ReporterObject, &Reporter_Type);
	if (ret == NULL)
		return NULL;
	ret->reporter = reporter;
	ret->report_baton = report_baton;
	ret->pool = temp_pool;
	return (PyObject *)ret;
}

static PyObject *ra_replay(PyObject *self, PyObject *args)
{
	RemoteAccessObject *ra = (RemoteAccessObject *)self;
	apr_pool_t *temp_pool;
	svn_revnum_t revision, low_water_mark;
	PyObject *update_editor;
	bool send_deltas = true;

	if (!PyArg_ParseTuple(args, "llO|b", &revision, &low_water_mark, &update_editor, &send_deltas))
		return NULL;

	temp_pool = Pool(ra->pool);
    RUN_SVN_WITH_POOL(temp_pool, 
					  svn_ra_replay(ra->ra, revision, low_water_mark,
									send_deltas, &py_editor, update_editor, 
									temp_pool));
	apr_pool_destroy(temp_pool);

	return Py_None;
}

static PyObject *ra_rev_proplist(PyObject *self, PyObject *args)
{
	apr_pool_t *temp_pool;
	apr_hash_t *props;
	RemoteAccessObject *ra = (RemoteAccessObject *)self;
	svn_revnum_t rev;
	PyObject *py_props;
	if (!PyArg_ParseTuple(args, "l", &rev))
		return NULL;

	temp_pool = Pool(ra->pool);
	RUN_SVN_WITH_POOL(temp_pool, 
					  svn_ra_rev_proplist(ra->ra, rev, &props, temp_pool));
	py_props = prop_hash_to_dict(props);
	apr_pool_destroy(temp_pool);
	return py_props;
}

static PyObject *get_commit_editor(PyObject *self, PyObject *args, PyObject *kwargs)
{
	char *kwnames[] = { "revprops", "callback", "lock_tokens", "keep_locks", 
		NULL };
	PyObject *revprops, *commit_callback, *lock_tokens = Py_None;
	bool keep_locks = false;
	apr_pool_t *temp_pool;
	const svn_delta_editor_t *editor;
	void *edit_baton;
	RemoteAccessObject *ra = (RemoteAccessObject *)self;
	apr_hash_t *hash_lock_tokens;

	if (!PyArg_ParseTupleAndKeywords(args, kwargs, "OO|Ob", kwnames, &revprops, &commit_callback, &lock_tokens, &keep_locks))
		return NULL;

	temp_pool = Pool(ra->pool);
	if (lock_tokens == Py_None) {
		hash_lock_tokens = NULL;
	} else {
		Py_ssize_t idx = 0;
		PyObject *k, *v;
		hash_lock_tokens = apr_hash_make(temp_pool);
		while (PyDict_Next(lock_tokens, &idx, &k, &v)) {
			apr_hash_set(hash_lock_tokens, PyString_AsString(k), 
						 PyString_Size(k), PyString_AsString(v));
		}
	}
	RUN_SVN_WITH_POOL(temp_pool, svn_ra_get_commit_editor2(ra->ra, &editor, 
		&edit_baton, 
		PyString_AsString(PyDict_GetItemString(revprops, SVN_PROP_REVISION_LOG)), py_commit_callback, 
		commit_callback, hash_lock_tokens, keep_locks, temp_pool));
	return new_editor_object(editor, edit_baton, temp_pool, 
								  &Editor_Type);
}

static PyObject *ra_change_rev_prop(PyObject *self, PyObject *args)
{
	svn_revnum_t rev;
	char *name;
	RemoteAccessObject *ra = (RemoteAccessObject *)self;
	char *value;
	int vallen;
    apr_pool_t *temp_pool;
 	svn_string_t *val_string;

	if (!PyArg_ParseTuple(args, "ss#", &name, &value, &vallen))
		return NULL;
	temp_pool = Pool(ra->pool);
	val_string = svn_string_ncreate(value, vallen, temp_pool);
	RUN_SVN_WITH_POOL(temp_pool, 
					  svn_ra_change_rev_prop(ra->ra, rev, name, val_string, 
											 temp_pool));
	apr_pool_destroy(temp_pool);
	return Py_None;
}
    
static PyObject *ra_get_dir(PyObject *self, PyObject *args)
{
   	apr_pool_t *temp_pool;
    apr_hash_t *dirents;
    apr_hash_index_t *idx;
    apr_hash_t *props;
    long fetch_rev;
    const char *key;
	RemoteAccessObject *ra = (RemoteAccessObject *)self;
    svn_dirent_t *dirent;
    apr_ssize_t klen;
	char *path;
	svn_revnum_t revision = -1;
	int dirent_fields = 0;
	PyObject *py_dirents, *py_props;

	if (!PyArg_ParseTuple(args, "s|li", &path, &revision, &dirent_fields))
		return NULL;

    temp_pool = Pool(ra->pool);

	if (!check_error(svn_ra_get_dir2(ra->ra, &dirents, &fetch_rev, &props,
                     path, revision, dirent_fields, temp_pool))) {
		apr_pool_destroy(temp_pool);
		return NULL;
	}

	if (dirents == NULL) {
		py_dirents = Py_None;
	} else {
		py_dirents = PyDict_New();
		idx = apr_hash_first(temp_pool, dirents);
		while (idx != NULL) {
			PyObject *py_dirent;
			apr_hash_this(idx, (const void **)&key, &klen, (void **)&dirent);
			py_dirent = PyDict_New();
			if (dirent_fields & 0x1)
            	PyDict_SetItemString(py_dirent, "kind", 
									 PyLong_FromLong(dirent->kind));
            if (dirent_fields & 0x2)
				PyDict_SetItemString(py_dirent, "size", 
									 PyLong_FromLong(dirent->size));
			if (dirent_fields & 0x4)
				PyDict_SetItemString(py_dirent, "has_props",
									 PyBool_FromLong(dirent->has_props));
			if (dirent_fields & 0x8)
				PyDict_SetItemString(py_dirent, "created_rev", 
									 PyLong_FromLong(dirent->created_rev));
			if (dirent_fields & 0x10)
				PyDict_SetItemString(py_dirent, "time", 
									 PyLong_FromLong(dirent->time));
			if (dirent_fields & 0x20)
				PyDict_SetItemString(py_dirent, "last_author",
									 PyString_FromString(dirent->last_author));
			PyDict_SetItemString(py_dirents, key, py_dirent);
			idx = apr_hash_next(idx);
		}
	}

	py_props = prop_hash_to_dict(props);
	apr_pool_destroy(temp_pool);
	return Py_BuildValue("(OiO)", py_dirents, fetch_rev, py_props);
}

static PyObject *ra_get_lock(PyObject *self, PyObject *args)
{
	char *path;
	RemoteAccessObject *ra = (RemoteAccessObject *)self;
	svn_lock_t *lock;
    apr_pool_t *temp_pool;

	if (!PyArg_ParseTuple(args, "s", &path))
		return NULL;

	temp_pool = Pool(ra->pool);
	RUN_SVN_WITH_POOL(temp_pool, 
				  svn_ra_get_lock(ra->ra, &lock, path, temp_pool));
	apr_pool_destroy(temp_pool);
	return wrap_lock(lock);
}

static PyObject *ra_check_path(PyObject *self, PyObject *args)
{
	char *path; 
	RemoteAccessObject *ra = (RemoteAccessObject *)self;
	svn_revnum_t revision;
	svn_node_kind_t kind;
    apr_pool_t *temp_pool;

	if (!PyArg_ParseTuple(args, "sl", &path, revision))
		return NULL;
	temp_pool = Pool(ra->pool);
    RUN_SVN_WITH_POOL(temp_pool, 
					  svn_ra_check_path(ra->ra, path, revision, &kind, 
                     temp_pool));
	apr_pool_destroy(temp_pool);
	return PyLong_FromLong(kind);
}

static PyObject *has_capability(PyObject *self, PyObject *args)
{
	char *capability;
	apr_pool_t *temp_pool;
	RemoteAccessObject *ra = (RemoteAccessObject *)self;
	int has;

	if (!PyArg_ParseTuple(args, "s", &capability))
		return NULL;
	
	temp_pool = Pool(ra->pool);
	RUN_SVN_WITH_POOL(temp_pool, 
					  svn_ra_has_capability(ra->ra, &has, capability, temp_pool));
	apr_pool_destroy(temp_pool);
	return PyBool_FromLong(has);
}

static PyObject *ra_unlock(PyObject *self, PyObject *args)
{
	RemoteAccessObject *ra = (RemoteAccessObject *)self;
	PyObject *path_tokens, *lock_func, *k, *v;
	bool break_lock;
	apr_ssize_t idx;
	apr_pool_t *temp_pool;
    apr_hash_t *hash_path_tokens;

	if (!PyArg_ParseTuple(args, "ObO", &path_tokens, &break_lock, &lock_func))
		return NULL;

	temp_pool = Pool(ra->pool);
	hash_path_tokens = apr_hash_make(temp_pool);
	while (PyDict_Next(path_tokens, &idx, &k, &v)) {
		apr_hash_set(hash_path_tokens, PyString_AsString(k), PyString_Size(k), (char *)PyString_AsString(v));
	}
	if (!check_error(svn_ra_unlock(ra->ra, hash_path_tokens, break_lock,
                     py_lock_func, lock_func, temp_pool))) {
		apr_pool_destroy(temp_pool);
		return NULL;
	}

    apr_pool_destroy(temp_pool);
	return Py_None;
}

static PyObject *ra_lock(PyObject *self, PyObject *args)
{
	RemoteAccessObject *ra = (RemoteAccessObject *)self;
	PyObject *path_revs;
	char *comment;
	int steal_lock;
	PyObject *lock_func, *k, *v;
 	apr_pool_t *temp_pool;
	apr_hash_t *hash_path_revs;
	svn_revnum_t *rev;
	Py_ssize_t idx = 0;

	if (!PyArg_ParseTuple(args, "OsbO", &path_revs, &comment, &steal_lock, 
						  &lock_func))
		return NULL;

   temp_pool = Pool(ra->pool);
	if (path_revs == Py_None) {
		hash_path_revs = NULL;
	} else {
		hash_path_revs = apr_hash_make(temp_pool);
	}

	while (PyDict_Next(path_revs, &idx, &k, &v)) {
		rev = (svn_revnum_t *)apr_palloc(temp_pool, sizeof(svn_revnum_t));
		*rev = PyLong_AsLong(v);
		apr_hash_set(hash_path_revs, PyString_AsString(k), PyString_Size(k), 
					 PyString_AsString(v));
	}
	if (!check_error(svn_ra_lock(ra->ra, hash_path_revs, comment, steal_lock,
                     py_lock_func, lock_func, temp_pool))) {
		apr_pool_destroy(temp_pool);
		return NULL;
	}
	apr_pool_destroy(temp_pool);
	return NULL;
}

static PyObject *ra_get_locks(PyObject *self, PyObject *args)
{
	char *path;
	apr_pool_t *temp_pool;
    apr_hash_t *hash_locks;
    apr_hash_index_t *idx;
	RemoteAccessObject *ra = (RemoteAccessObject *)self;
    char *key;
    apr_ssize_t klen;
    svn_lock_t *lock;
	PyObject *ret;

	if (!PyArg_ParseTuple(args, "s", &path))
		return NULL;

	temp_pool = Pool(ra->pool);
	if (!check_error(svn_ra_get_locks(ra->ra, &hash_locks, path, temp_pool))) {
		apr_pool_destroy(temp_pool);
		return NULL;
	}

    ret = PyDict_New();
	for (idx = apr_hash_first(temp_pool, hash_locks); idx != NULL;
         idx = apr_hash_next(idx)) {
		apr_hash_this(idx, (const void **)&key, &klen, (void **)&lock);
		PyDict_SetItemString(ret, key, pyify_lock(lock));
	}

	apr_pool_destroy(temp_pool);
	return ret;
}

static PyObject *ra_get_locations(PyObject *self, PyObject *args)
{
	char *path;
	RemoteAccessObject *ra = (RemoteAccessObject *)self;
	svn_revnum_t peg_revision;
	PyObject *location_revisions;
    apr_pool_t *temp_pool;
    apr_hash_t *hash_locations;
    apr_hash_index_t *idx;
    svn_revnum_t *key;
	PyObject *ret;
    apr_ssize_t klen;
    char *val;

	if (!PyArg_ParseTuple(args, "slO", &path, &peg_revision, &location_revisions))
		return NULL;

    temp_pool = Pool(NULL);
    if (!check_error(svn_ra_get_locations(ra->ra, &hash_locations,
                    path, peg_revision, 
                    revnum_list_to_apr_array(temp_pool, location_revisions),
                    temp_pool))) {
		apr_pool_destroy(temp_pool);
		return NULL;
	}
	ret = PyDict_New();

    for (idx = apr_hash_first(temp_pool, hash_locations); idx != NULL; 
		idx = apr_hash_next(idx)) {
		apr_hash_this(idx, (const void **)&key, &klen, (void **)&val);
		PyDict_SetItem(ret, PyLong_FromLong(*key), PyString_FromString(val));
	}
	apr_pool_destroy(temp_pool);
	return ret;
}
    
static PyObject *ra_get_file_revs(PyObject *self, PyObject *args)
{
	char *path;
	svn_revnum_t start, end;
	PyObject *file_rev_handler;
	apr_pool_t *temp_pool;
	RemoteAccessObject *ra = (RemoteAccessObject *)self;

	if (!PyArg_ParseTuple(args, "sllO", &path, &start, &end, &file_rev_handler))
		return NULL;

	temp_pool = Pool(ra->pool);

	if (!check_error(svn_ra_get_file_revs(ra->ra, path, start, end, 
				py_file_rev_handler, (void *)file_rev_handler, 
                    temp_pool))) {
		apr_pool_destroy(temp_pool);
		return NULL;
	}

	apr_pool_destroy(temp_pool);

	return Py_None;
}

static void ra_dealloc(PyObject *self)
{
	RemoteAccessObject *ra = (RemoteAccessObject *)self;
	Py_DECREF(ra->url);
	apr_pool_destroy(ra->pool);
	Py_DECREF(ra->auth);
}

static PyObject *ra_repr(PyObject *self)
{
	RemoteAccessObject *ra = (RemoteAccessObject *)self;
	return PyString_FromFormat("RemoteAccess(%s)", PyString_AsString(ra->url));
}

typedef struct { 
	PyObject_HEAD
    apr_pool_t *pool;
    svn_auth_provider_object_t *provider;
} AuthProviderObject;

static void auth_provider_dealloc(PyObject *self)
{
	AuthProviderObject *auth_provider = (AuthProviderObject *)self;
	apr_pool_destroy(auth_provider->pool);
}


static PyObject *auth_init(PyTypeObject *type, PyObject *args, PyObject *kwargs)
{
	char *kwnames[] = { "providers", NULL };
	apr_array_header_t *c_providers;
	svn_auth_provider_object_t **el;
	PyObject *providers = Py_None;
	AuthObject *ret;
	int i;

	if (!PyArg_ParseTupleAndKeywords(args, kwargs, "|O", kwnames, &providers))
		return NULL;

	ret = PyObject_New(AuthObject, &Auth_Type);
	if (ret == NULL)
		return NULL;

	ret->pool = Pool(NULL);
	ret->providers = providers;
	Py_INCREF(providers);

    c_providers = apr_array_make(ret->pool, PyList_Size(providers), 4);
	for (i = 0; i < PyList_Size(providers); i++) {
		AuthProviderObject *provider;
    	el = (svn_auth_provider_object_t **)apr_array_push(c_providers);
		/* FIXME: Check that provider is indeed a AuthProviderObject object */
        provider = (AuthProviderObject *)PyList_GetItem(providers, i);
        *el = provider->provider;
	}
	svn_auth_open(&ret->auth_baton, c_providers, ret->pool);
	return (PyObject *)ret;
}

static PyObject *auth_set_parameter(PyObject *self, PyObject *args)
{
	AuthObject *auth = (AuthObject *)self;
	char *name, *value;
	if (!PyArg_ParseTuple(args, "ss", &name, &value)) {
        svn_auth_set_parameter(auth->auth_baton, name, (char *)value);
	}

	return Py_None;
}

static PyObject *auth_get_parameter(PyObject *self, PyObject *args)
{
	char *name;
	AuthObject *auth = (AuthObject *)self;

	if (!PyArg_ParseTuple(args, "s", &name))
		return NULL;

	return PyString_FromString(svn_auth_get_parameter(auth->auth_baton, name));
}

static PyMethodDef auth_methods[] = {
	{ "set_parameter", auth_set_parameter, METH_VARARGS, NULL },
	{ "get_parameter", auth_get_parameter, METH_VARARGS, NULL },
	{ NULL }
};

static void auth_dealloc(PyObject *self)
{
	AuthObject *auth = (AuthObject *)self;
	apr_pool_destroy(auth->pool);
	Py_DECREF(auth->providers);	
}

static svn_error_t *py_username_prompt(svn_auth_cred_username_t **cred, void *baton, const char *realm, int may_save, apr_pool_t *pool)
{
    PyObject *fn = (PyObject *)baton, *ret;
	ret = PyObject_CallFunction(fn, "sb", realm, may_save);
	if (ret == NULL)
		return py_svn_error();
	(*cred)->username = apr_pstrdup(pool, PyString_AsString(PyTuple_GetItem(ret, 0)));
	(*cred)->may_save = (PyTuple_GetItem(ret, 1) == Py_True);
    return NULL;
}

static PyObject *get_username_prompt_provider(PyObject *self, PyObject *args)
{
    AuthProviderObject *auth;
	PyObject *prompt_func;
	int retry_limit;
	if (!PyArg_ParseTuple(args, "Oi", &prompt_func, &retry_limit))
		return NULL;
    auth = PyObject_New(AuthProviderObject, &AuthProvider_Type);
    auth->pool = Pool(NULL);
    svn_auth_get_username_prompt_provider(&auth->provider, py_username_prompt, (void *)prompt_func, retry_limit, auth->pool);
    return (PyObject *)auth;
}

static svn_error_t *py_simple_prompt(svn_auth_cred_simple_t **cred, void *baton, const char *realm, const char *username, int may_save, apr_pool_t *pool)
{
    PyObject *fn = (PyObject *)baton, *ret;
	ret = PyObject_CallFunction(fn, "sb", realm, may_save);
	if (ret == NULL)
		return py_svn_error();
	/* FIXME: Check type of ret */
    (*cred)->username = apr_pstrdup(pool, PyString_AsString(PyTuple_GetItem(ret, 0)));
    (*cred)->password = apr_pstrdup(pool, PyString_AsString(PyTuple_GetItem(ret, 1)));
	(*cred)->may_save = (PyTuple_GetItem(ret, 2) == Py_True);
    return NULL;
}

static PyObject *get_simple_prompt_provider(PyObject *self, PyObject *args)
{
	PyObject *prompt_func;
	int retry_limit;
    AuthProviderObject *auth;

	if (!PyArg_ParseTuple(args, "Oi", &prompt_func, &retry_limit))
		return NULL;

    auth = AuthProviderObject();
    auth->pool = Pool(NULL);
    svn_auth_get_simple_prompt_provider (&auth->provider, py_simple_prompt, (void *)prompt_func, retry_limit, auth->pool);
    return (PyObject *)auth;
}

static svn_error_t *py_ssl_server_trust_prompt(svn_auth_cred_ssl_server_trust_t **cred, void *baton, const char *realm, apr_uint32_t failures, svn_auth_ssl_server_cert_info_t *cert_info, svn_boolean_t may_save, apr_pool_t *pool)
{
    PyObject *fn = (PyObject *)baton;
	PyObject *ret;

	ret = PyObject_CallFunction(fn, "sl(ssssss)b", realm, failures, 
						  cert_info->hostname, cert_info->fingerprint, 
						  cert_info->valid_from, cert_info->valid_until, 
						  cert_info->issuer_dname, cert_info->ascii_cert, 
						  may_save);
	if (ret == NULL)
		return py_svn_error();

	/* FIXME: Check that ret is a tuple of size 2 */

	(*cred)->may_save = (PyTuple_GetItem(ret, 0) == Py_True);
	(*cred)->accepted_failures = PyLong_AsLong(PyTuple_GetItem(ret, 1));

    return NULL;
}

PyObject *get_ssl_server_trust_prompt_provider(PyObject *self, PyObject *args)
{
    AuthProviderObject *auth;
	PyObject *prompt_func;

	if (!PyArg_ParseTuple(args, "O", &prompt_func))
		return NULL;

    auth = PyObject_New(AuthProviderObject, &AuthProvider_Type);
	if (auth == NULL)
		return NULL;
    auth->pool = Pool(NULL);
    svn_auth_get_ssl_server_trust_prompt_provider (&auth->provider, py_ssl_server_trust_prompt, (void *)prompt_func, auth->pool);
    return (PyObject *)auth;
}

svn_error_t *py_ssl_client_cert_pw_prompt(svn_auth_cred_ssl_client_cert_pw_t **cred, void *baton, const char *realm, svn_boolean_t may_save, apr_pool_t *pool)
{
    PyObject *fn = (PyObject *)baton, *ret;
	ret = PyObject_CallFunction(fn, "sb", realm, may_save);
	if (ret == NULL) 
		return py_svn_error();
	/* FIXME: Check ret is a tuple of size 2 */
	(*cred)->password = apr_pstrdup(pool, PyString_AsString(PyTuple_GetItem(ret, 0)));
	(*cred)->may_save = (PyTuple_GetItem(ret, 1) == Py_True);
    return NULL;
}

PyObject *get_ssl_client_cert_pw_prompt_provider(PyObject *self, PyObject *args)
{
	PyObject *prompt_func;
	int retry_limit;
    AuthProviderObject *auth;

	if (!PyArg_ParseTuple(args, "Oi", &prompt_func, &retry_limit))
		return NULL;

    auth = PyObject_New(AuthProviderObject, &AuthProvider_Type);
	if (auth == NULL)
		return NULL;
    auth->pool = Pool(NULL);
    svn_auth_get_ssl_client_cert_pw_prompt_provider (&auth->provider, py_ssl_client_cert_pw_prompt, (void *)prompt_func, retry_limit, auth->pool);
    return (PyObject *)auth;
}

PyObject *get_username_provider(PyObject *self)
{
    AuthProviderObject *auth;
    auth = PyObject_New(AuthProviderObject, &AuthProvider_Type);
	if (auth == NULL)
		return NULL;
    auth->pool = Pool(NULL);
    svn_auth_get_username_provider(&auth->provider, auth->pool);
    return (PyObject *)auth;
}

static PyObject *get_simple_provider(PyObject *self)
{
    AuthProviderObject *auth = PyObject_New(AuthProviderObject, 
											&AuthProvider_Type);
    auth->pool = Pool(NULL);
    svn_auth_get_simple_provider(&auth->provider, auth->pool);
    return (PyObject *)auth;
}

static PyObject *get_ssl_server_trust_file_provider(PyObject *self)
{
    AuthProviderObject *auth = PyObject_New(AuthProviderObject, &AuthProvider_Type);
    auth->pool = Pool(NULL);
    svn_auth_get_ssl_server_trust_file_provider(&auth->provider, auth->pool);
    return (PyObject *)auth;
}

static PyObject *get_ssl_client_cert_file_provider(PyObject *self)
{
    AuthProviderObject *auth = PyObject_New(AuthProviderObject, &AuthProvider_Type);
    auth->pool = Pool(NULL);
    svn_auth_get_ssl_client_cert_file_provider(&auth->provider, auth->pool);
    return (PyObject *)auth;
}

static PyObject *get_ssl_client_cert_pw_file_provider(PyObject *self)
{
    AuthProviderObject *auth = PyObject_New(AuthProviderObject, &AuthProvider_Type);
    auth->pool = Pool(NULL);
    svn_auth_get_ssl_client_cert_pw_file_provider(&auth->provider, auth->pool);
    return (PyObject *)auth;
}

static PyObject *txdelta_send_stream(PyObject *self, PyObject *args)
{
    unsigned char digest[16];
    apr_pool_t *pool = Pool(NULL);
	PyObject *stream, *handler;

	if (!PyArg_ParseTuple(args, "OO", &stream, &handle))
		return NULL;

    if (!check_error(svn_txdelta_send_stream(new_py_stream(pool, stream), handler.txdelta, handler.txbaton, (unsigned char *)digest, pool))) {
		apr_pool_destroy(pool);
		return NULL;
	}
    apr_pool_destroy(pool);
    return PyString_FromStringAndSize((char *)digest, 16);
}

static PyMethodDef ra_methods[] = {
	{ "version", (PyCFunction)version, METH_NOARGS, NULL },
	{ "get_ssl_client_cert_pw_file_provider", (PyCFunction)get_ssl_client_cert_pw_file_provider, METH_NOARGS, NULL },
	{ "get_ssl_client_cert_file_provider", (PyCFunction)get_ssl_client_cert_file_provider, METH_NOARGS, NULL },
	{ NULL }
};

void initra(void)
{
	PyObject *mod;
	apr_initialize();

	mod = Py_InitModule3("ra", ra_methods, "Remote Access");
	if (mod == NULL)
		return;
}
