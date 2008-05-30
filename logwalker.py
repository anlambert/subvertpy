# Copyright (C) 2006 Jelmer Vernooij <jelmer@samba.org>

# This program is free software; you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation; either version 3 of the License, or
# (at your option) any later version.

# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.

# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software
# Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
"""Cache of the Subversion history log."""

from bzrlib import urlutils
from bzrlib.errors import NoSuchRevision
from bzrlib.trace import mutter
import bzrlib.ui as ui

from core import SubversionException
from transport import SvnRaTransport
import core
import constants

from cache import CacheTable
import changes

LOG_CHUNK_LIMIT = 0

class lazy_dict(object):
    def __init__(self, initial, create_fn, *args):
        self.initial = initial
        self.create_fn = create_fn
        self.args = args
        self.dict = None

    def _ensure_init(self):
        if self.dict is None:
            self.dict = self.create_fn(*self.args)
            self.create_fn = None

    def __len__(self):
        self._ensure_init()
        return len(self.dict)

    def __getitem__(self, key):
        if key in self.initial:
            return self.initial.__getitem__(key)
        self._ensure_init()
        return self.dict.__getitem__(key)

    def __setitem__(self, key, value):
        self._ensure_init()
        return self.dict.__setitem__(key, value)

    def __contains__(self, key):
        if key in self.initial:
            return True
        self._ensure_init()
        return self.dict.__contains__(key)

    def get(self, key, default=None):
        if key in self.initial:
            return self.initial[key]
        self._ensure_init()
        return self.dict.get(key, default)

    def has_key(self, key):
        if self.initial.has_key(key):
            return True
        self._ensure_init()
        return self.dict.has_key(key)

    def keys(self):
        self._ensure_init()
        return self.dict.keys()

    def values(self):
        self._ensure_init()
        return self.dict.values()

    def items(self):
        self._ensure_init()
        return self.dict.items()

    def __repr__(self):
        self._ensure_init()
        return repr(self.dict)

    def __eq__(self, other):
        self._ensure_init()
        return self.dict.__eq__(other)


class CachingLogWalker(CacheTable):
    """Subversion log browser."""
    def __init__(self, actual, cache_db=None):
        CacheTable.__init__(self, cache_db)

        self.actual = actual
        self._transport = actual._transport
        self.find_children = actual.find_children

        self.saved_revnum = self.cachedb.execute("SELECT MAX(rev) FROM changed_path").fetchone()[0]
        if self.saved_revnum is None:
            self.saved_revnum = 0

    def _create_table(self):
        self.cachedb.executescript("""
          create table if not exists changed_path(rev integer, action text, path text, copyfrom_path text, copyfrom_rev integer);
          create index if not exists path_rev on changed_path(rev);
          create unique index if not exists path_rev_path on changed_path(rev, path);
          create unique index if not exists path_rev_path_action on changed_path(rev, path, action);
        """)

    def find_latest_change(self, path, revnum):
        """Find latest revision that touched path.

        :param path: Path to check for changes
        :param revnum: First revision to check
        """
        assert isinstance(path, str)
        assert isinstance(revnum, int) and revnum >= 0
        self.fetch_revisions(revnum)

        self.mutter("latest change: %r:%r" % (path, revnum))

        extra = ""
        if path == "":
            extra += " OR path LIKE '%'"
        else:
            extra += " OR path LIKE '%s/%%'" % path.strip("/")
        extra += " OR ('%s' LIKE (path || '/%%') AND (action = 'R' OR action = 'A'))" % path.strip("/")
 
        query = "SELECT rev FROM changed_path WHERE (path='%s'%s) AND rev <= %d ORDER BY rev DESC LIMIT 1" % (path.strip("/"), extra, revnum)

        row = self.cachedb.execute(query).fetchone()
        if row is None and path == "":
            return 0

        if row is None:
            return None

        return row[0]

    def iter_changes(self, paths, from_revnum, to_revnum=0, limit=0, pb=None):
        """Return iterator over all the revisions between from_revnum and to_revnum named path or inside path.

        :param paths:    Paths to report about.
        :param from_revnum:  Start revision.
        :param to_revnum: End revision.
        :return: An iterator that yields tuples with (paths, revnum, revprops)
            where paths is a dictionary with all changes that happened 
            in revnum.
        """
        assert from_revnum >= 0 and to_revnum >= 0

        ascending = (to_revnum > from_revnum)

        revnum = from_revnum

        self.mutter("iter changes %r->%r (%r)" % (from_revnum, to_revnum, paths))

        if paths is None:
            path = ""
        else:
            assert len(paths) == 1
            path = paths[0].strip("/")

        assert from_revnum >= to_revnum or path == ""

        i = 0

        while ((not ascending and revnum >= to_revnum) or
               (ascending and revnum <= to_revnum)):
            if pb is not None:
                pb.update("determining changes", from_revnum-revnum, from_revnum)
            assert revnum > 0 or path == "", "Inconsistent path,revnum: %r,%r" % (revnum, path)
            revpaths = self._get_revision_paths(revnum)

            if ascending:
                next = (path, revnum+1)
            else:
                next = changes.find_prev_location(revpaths, path, revnum)

            revprops = lazy_dict({}, self._transport.revprop_list, revnum)

            if changes.changes_path(revpaths, path, True):
                yield (revpaths, revnum, revprops)
                i += 1
                if limit != 0 and i == limit:
                    break

            if next is None:
                break

            (path, revnum) = next

    def get_previous(self, path, revnum):
        """Return path,revnum pair specified pair was derived from.

        :param path:  Path to check
        :param revnum:  Revision to check
        """
        assert revnum >= 0
        self.fetch_revisions(revnum)
        self.mutter("get previous %r:%r" % (path, revnum))
        if revnum == 0:
            return (None, -1)
        row = self.cachedb.execute("select action, copyfrom_path, copyfrom_rev from changed_path where path='%s' and rev=%d" % (path, revnum)).fetchone()
        if row is None:
            return (None, -1)
        if row[2] == -1:
            if row[0] == 'A':
                return (None, -1)
            return (path, revnum-1)
        return (row[1], row[2])

    def _get_revision_paths(self, revnum):
        if revnum == 0:
            return {'': ('A', None, -1)}

        self.fetch_revisions(revnum)

        query = "select path, action, copyfrom_path, copyfrom_rev from changed_path where rev="+str(revnum)

        paths = {}
        for p, act, cf, cr in self.cachedb.execute(query):
            if cf is not None:
                cf = cf.encode("utf-8")
            paths[p.encode("utf-8")] = (act, cf, cr)
        return paths

    def get_revision_paths(self, revnum):
        """Obtain dictionary with all the changes in a particular revision.

        :param revnum: Subversion revision number
        :returns: dictionary with paths as keys and 
                  (action, copyfrom_path, copyfrom_rev) as values.
        """
        self.mutter("revision paths: %r" % revnum)

        return self._get_revision_paths(revnum)

    def fetch_revisions(self, to_revnum=None):
        """Fetch information about all revisions in the remote repository
        until to_revnum.

        :param to_revnum: End of range to fetch information for
        """
        if to_revnum <= self.saved_revnum:
            return
        latest_revnum = self.actual._transport.get_latest_revnum()
        to_revnum = max(latest_revnum, to_revnum)

        pb = ui.ui_factory.nested_progress_bar()

        try:
            try:
                while self.saved_revnum < to_revnum:
                    for (orig_paths, revision, revprops) in self.actual._transport.iter_log(None, self.saved_revnum, 
                                             to_revnum, self.actual._limit, True, 
                                             True, []):
                        pb.update('fetching svn revision info', revision, to_revnum)
                        if orig_paths is None:
                            orig_paths = {}
                        for p in orig_paths:
                            copyfrom_path = orig_paths[p].copyfrom_path
                            if copyfrom_path is not None:
                                copyfrom_path = copyfrom_path.strip("/")

                            self.cachedb.execute(
                                 "replace into changed_path (rev, path, action, copyfrom_path, copyfrom_rev) values (?, ?, ?, ?, ?)", 
                                 (revision, p.strip("/"), orig_paths[p].action, copyfrom_path, orig_paths[p].copyfrom_rev))
                            # Work around nasty memory leak in Subversion
                            orig_paths[p]._parent_pool.destroy()

                        self.saved_revnum = revision
                        if self.saved_revnum % 1000 == 0:
                            self.cachedb.commit()
            finally:
                pb.finished()
        except SubversionException, (_, num):
            if num == constants.ERR_FS_NO_SUCH_REVISION:
                raise NoSuchRevision(branch=self, 
                    revision="Revision number %d" % to_revnum)
            raise
        self.cachedb.commit()


def struct_revpaths_to_tuples(changed_paths):
    assert isinstance(changed_paths, dict)
    revpaths = {}
    for k,v in changed_paths.items():
        if v.copyfrom_path is None:
            copyfrom_path = None
        else:
            copyfrom_path = v.copyfrom_path.strip("/")
        revpaths[k.strip("/")] = (v.action, copyfrom_path, v.copyfrom_rev)
    return revpaths


class LogWalker(object):
    """Easy way to access the history of a Subversion repository."""
    def __init__(self, transport, limit=None):
        """Create a new instance.

        :param transport:   SvnRaTransport to use to access the repository.
        """
        assert isinstance(transport, SvnRaTransport)

        self._transport = transport

        if limit is not None:
            self._limit = limit
        else:
            self._limit = LOG_CHUNK_LIMIT

    def find_latest_change(self, path, revnum):
        """Find latest revision that touched path.

        :param path: Path to check for changes
        :param revnum: First revision to check
        """
        assert isinstance(path, str)
        assert isinstance(revnum, int) and revnum >= 0

        try:
            return self._transport.iter_log([path], revnum, 0, 2, True, False, []).next()[1]
        except SubversionException, (_, num):
            if num == svn.core.SVN_ERR_FS_NO_SUCH_REVISION:
                raise NoSuchRevision(branch=self, 
                    revision="Revision number %d" % revnum)
            if num == svn.core.SVN_ERR_FS_NOT_FOUND:
                return None
            raise

    def iter_changes(self, paths, from_revnum, to_revnum=0, limit=0, pb=None):
        """Return iterator over all the revisions between revnum and 0 named path or inside path.

        :param paths:    Paths report about (in revnum)
        :param from_revnum:  Start revision.
        :param to_revnum: End revision.
        :return: An iterator that yields tuples with (paths, revnum, revprops)
            where paths is a dictionary with all changes that happened in revnum.
        """
        assert from_revnum >= 0 and to_revnum >= 0

        try:
            for (changed_paths, revnum, known_revprops) in self._transport.iter_log(paths, from_revnum, to_revnum, limit, True, False, []):
                if pb is not None:
                    pb.update("determining changes", from_revnum-revnum, from_revnum)
                if revnum == 0 and changed_paths is None:
                    revpaths = {"": ('A', None, -1)}
                else:
                    assert isinstance(changed_paths, dict), "invalid paths in %r:%r" % (revnum, path)
                    revpaths = struct_revpaths_to_tuples(changed_paths)
                revprops = lazy_dict(known_revprops, self._transport.revprop_list, revnum)
                yield (revpaths, revnum, revprops)
        except SubversionException, (_, num):
            if num == svn.core.SVN_ERR_FS_NO_SUCH_REVISION:
                raise NoSuchRevision(branch=self, 
                    revision="Revision number %d" % from_revnum)
            raise

    def get_revision_paths(self, revnum):
        """Obtain dictionary with all the changes in a particular revision.

        :param revnum: Subversion revision number
        :returns: dictionary with paths as keys and 
                  (action, copyfrom_path, copyfrom_rev) as values.
        """
        # To make the existing code happy:
        if revnum == 0:
            return {'': ('A', None, -1)}

        try:
            return struct_revpaths_to_tuples(
                self._transport.iter_log(None, revnum, revnum, 1, True, True, []).next()[0])
        except SubversionException, (_, num):
            if num == svn.core.SVN_ERR_FS_NO_SUCH_REVISION:
                raise NoSuchRevision(branch=self, 
                    revision="Revision number %d" % revnum)
            raise
        
    def find_children(self, path, revnum):
        """Find all children of path in revnum.

        :param path:  Path to check
        :param revnum:  Revision to check
        """
        assert isinstance(path, str), "invalid path"
        path = path.strip("/")
        conn = self._transport.connections.get(self._transport.get_svn_repos_root())
        try:
            ft = conn.check_path(path, revnum)
            if ft == svn.core.svn_node_file:
                return []
            assert ft == svn.core.svn_node_dir
        finally:
            self._transport.connections.add(conn)

        class TreeLister(svn.delta.Editor):
            def __init__(self, base):
                self.files = []
                self.base = base

            def set_target_revision(self, revnum):
                """See Editor.set_target_revision()."""
                pass

            def open_root(self, revnum):
                """See Editor.open_root()."""
                return path

            def add_directory(self, path, parent_baton, copyfrom_path, copyfrom_revnum, pool):
                """See Editor.add_directory()."""
                self.files.append(urlutils.join(self.base, path))
                return path

            def change_dir_prop(self, id, name, value, pool):
                pass

            def change_file_prop(self, id, name, value, pool):
                pass

            def add_file(self, path, parent_id, copyfrom_path, copyfrom_revnum, baton):
                self.files.append(urlutils.join(self.base, path))
                return path

            def close_dir(self, id):
                pass

            def close_file(self, path, checksum):
                pass

            def close_edit(self):
                pass

            def abort_edit(self):
                pass

            def apply_textdelta(self, file_id, base_checksum):
                pass

        editor = TreeLister(path)
        try:
            conn = self._transport.connections.get(urlutils.join(self._transport.get_svn_repos_root(), path))
            reporter = conn.do_update(revnum, True, editor, pool)
            reporter.set_path("", revnum, True, None, pool)
            reporter.finish_report(pool)
        finally:
            self._transport.connections.add(conn)
        return editor.files

    def get_previous(self, path, revnum):
        """Return path,revnum pair specified pair was derived from.

        :param path:  Path to check
        :param revnum:  Revision to check
        """
        assert revnum >= 0
        if revnum == 0:
            return (None, -1)

        try:
            paths = struct_revpaths_to_tuples(self._transport.iter_log([path], revnum, revnum, 1, True, False, []).next()[0])
        except SubversionException, (_, num):
            if num == svn.core.SVN_ERR_FS_NO_SUCH_REVISION:
                raise NoSuchRevision(branch=self, 
                    revision="Revision number %d" % revnum)
            raise

        if not path in paths:
            return (None, -1)

        if paths[path][2] == -1:
            if paths[path][0] == 'A':
                return (None, -1)
            return (path, revnum-1)

        return (paths[path][1], paths[path][2])

