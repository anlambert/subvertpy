# Copyright (C) 2005-2007 Jelmer Vernooij <jelmer@samba.org>
 
# This program is free software; you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation; either version 3 of the License, or
# (at your option) any later version.

# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.

# You should have received a copy of the GNU General Public License
# along with this program.  If not, see <http://www.gnu.org/licenses/>.

"""Subversion ra library tests."""

from bzrlib.tests import TestCase
from bzrlib.plugins.svn import ra
from bzrlib.plugins.svn.tests import TestCaseWithSubversionRepository

class VersionTest(TestCase):
    def test_version_length(self):
        self.assertEquals(4, len(ra.version()))

class TestRemoteAccess(TestCaseWithSubversionRepository):
    def setUp(self):
        super(TestRemoteAccess, self).setUp()
        self.repos_url = self.make_client("d", "dc")
        self.ra = ra.RemoteAccess(self.repos_url)

    def do_commit(self):
        self.build_tree({'dc/foo': None})
        self.client_add("dc/foo")
        self.client_commit("dc", "msg")

    def test_repr(self):
        self.assertEquals("RemoteAccess(%s)" % self.repos_url,
                          repr(self.ra))

    def test_latest_revnum(self):
        self.assertEquals(0, self.ra.get_latest_revnum())

    def test_latest_revnum_one(self):
        self.do_commit()
        self.assertEquals(1, self.ra.get_latest_revnum())

    def test_get_uuid(self):
        self.assertIsInstance(self.ra.get_uuid(), str)

    def test_get_repos_root(self):
        self.assertEqual(self.repos_url, self.ra.get_repos_root())

    def test_reparent(self):
        self.ra.reparent(self.repos_url)

    def test_has_capability(self):
        self.assertRaises(NotImplementedError, self.ra.has_capability, "FOO")

    def test_get_dir(self):
        ret = self.ra.get_dir("", 0)
        self.assertIsInstance(ret, tuple)

    def test_change_rev_prop(self):
        self.do_commit()
        self.ra.change_rev_prop(1, "foo", "bar")

    def test_rev_proplist(self):
        self.assertIsInstance(self.ra.rev_proplist(0), dict)

    def test_get_log(self):
        returned = []
        def cb(*args):
            returned.append(args)
        self.ra.get_log(cb, [""], 0, 0)
        self.assertEquals(1, len(returned))
        self.do_commit()
        returned = []
        self.ra.get_log(cb, ["/"], 0, 1, discover_changed_paths=True, 
                        strict_node_history=False)
        self.assertEquals(2, len(returned))
        (paths, revnum, props) = returned[0]
        self.assertEquals(None, paths)
        self.assertEquals(revnum, 0)
        self.assertEquals(["svn:date"], props.keys())
        (paths, revnum, props) = returned[1]
        self.assertEquals({'/foo': ('A', None, -1)}, paths)
        self.assertEquals(revnum, 1)
        self.assertEquals(set(["svn:date", "svn:author", "svn:log"]), 
                          set(props.keys()))

    def test_get_commit_editor(self):
        def mycb(rev):
            pass
        editor = self.ra.get_commit_editor({"svn:log": "foo"}, mycb)
        self.assertRaises(ra.BusyException, self.ra.get_commit_editor, {"svn:log": "foo"}, mycb)
        editor.abort()

