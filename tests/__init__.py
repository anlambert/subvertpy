# Copyright (C) 2006-2007 Jelmer Vernooij <jelmer@samba.org>

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

"""Tests for the bzr-svn plugin."""

import os
import sys
import bzrlib
from bzrlib import osutils
from bzrlib.bzrdir import BzrDir
from bzrlib.tests import TestCaseInTempDir, TestSkipped
from bzrlib.trace import mutter
from bzrlib.urlutils import local_path_to_url
from bzrlib.workingtree import WorkingTree

import constants
import repos, wc, client

class TestCaseWithSubversionRepository(TestCaseInTempDir):
    """A test case that provides the ability to build Subversion 
    repositories."""

    def setUp(self):
        super(TestCaseWithSubversionRepository, self).setUp()
        self.client_ctx = client.Client()
        self.client_ctx.set_log_msg_func(self.log_message_func)

    def log_message_func(self, items):
        return (self.next_message, None)

    def make_repository(self, relpath, allow_revprop_changes=True):
        """Create a repository.

        :return: Handle to the repository.
        """
        abspath = os.path.join(self.test_dir, relpath)

        repos.create(abspath)

        if allow_revprop_changes:
            if sys.platform == 'win32':
                revprop_hook = os.path.join(abspath, "hooks", "pre-revprop-change.bat")
                open(revprop_hook, 'w').write("exit 0\n")
            else:
                revprop_hook = os.path.join(abspath, "hooks", "pre-revprop-change")
                open(revprop_hook, 'w').write("#!/bin/sh\n")
                os.chmod(revprop_hook, os.stat(revprop_hook).st_mode | 0111)

        return local_path_to_url(abspath)

    def make_remote_bzrdir(self, relpath):
        """Create a repository."""

        repos_url = self.make_repository(relpath)

        return BzrDir.open("svn+%s" % repos_url)

    def open_local_bzrdir(self, repos_url, relpath):
        """Open a local BzrDir."""

        self.make_checkout(repos_url, relpath)

        return BzrDir.open(relpath)

    def make_local_bzrdir(self, repos_path, relpath):
        """Create a repository and checkout."""

        repos_url = self.make_repository(repos_path)

        return self.open_local_bzrdir(repos_url, relpath)


    def make_checkout(self, repos_url, relpath):
        self.client_ctx.checkout(repos_url, relpath, "HEAD", "HEAD", 
                                 True, False)

    @staticmethod
    def create_checkout(branch, path, revision_id=None, lightweight=False):
        return branch.create_checkout(path, revision_id=revision_id,
                                          lightweight=lightweight)

    @staticmethod
    def open_checkout(url):
        return WorkingTree.open(url)

    @staticmethod
    def open_checkout_bzrdir(url):
        return BzrDir.open(url)

    @staticmethod
    def create_branch_convenience(url):
        return BzrDir.create_branch_convenience(url)

    def client_set_prop(self, path, name, value):
        if value is None:
            value = ""
        self.client_ctx.propset(name, value, path, False, True)

    def client_get_prop(self, path, name, revnum=None, recursive=False):
        if revnum is None:
            revnum = "WORKING"
        ret = self.client_ctx.propget(name, path, revnum, revnum, recursive)
        if recursive:
            return ret
        else:
            return ret.values()[0]

    def client_get_revprop(self, url, revnum, name):
        return self.client_ctx.revprop_get(name, url, revnum)[0]

    def client_set_revprop(self, url, revnum, name, value):
        return self.client_ctx.revprop_set(name, url, renum, value)
        
    def client_commit(self, dir, message=None, recursive=True):
        """Commit current changes in specified working copy.
        
        :param relpath: List of paths to commit.
        """
        olddir = os.path.abspath('.')
        self.next_message = message
        os.chdir(dir)
        info = self.client_ctx.commit(["."], recursive, False)
        os.chdir(olddir)
        assert info is not None
        return info

    def client_add(self, relpath, recursive=True):
        """Add specified files to working copy.
        
        :param relpath: Path to the files to add.
        """
        self.client_ctx.add(relpath, recursive, False, False)

    def client_log(self, path, start_revnum=0, stop_revnum="HEAD"):
        assert isinstance(path, str)
        ret = {}
        def rcvr(orig_paths, rev, revprops):
            ret[rev] = (orig_paths, 
                        revprops.get(constants.PROP_REVISION_AUTHOR),
                        revprops.get(constants.PROP_REVISION_DATE),
                        revprops.get(constants.PROP_REVISION_LOG))
        self.client_ctx.log([path], rcvr, None, start_revnum, stop_revnum, 
                            True, True)
        return ret

    def client_delete(self, relpath):
        """Remove specified files from working copy.

        :param relpath: Path to the files to remove.
        """
        self.client_ctx.delete([relpath], True)

    def client_copy(self, oldpath, newpath, revnum=None):
        """Copy file in working copy.

        :param oldpath: Relative path to original file.
        :param newpath: Relative path to new file.
        """
        self.client_ctx.copy(oldpath, newpath, revnum)

    def client_update(self, path):
        self.client_ctx.update(path, None, True)

    def build_tree(self, files):
        """Create a directory tree.
        
        :param files: Dictionary with filenames as keys, contents as 
            values. None as value indicates a directory.
        """
        for f in files:
            if files[f] is None:
                try:
                    os.makedirs(f)
                except OSError:
                    pass
            else:
                try:
                    os.makedirs(os.path.dirname(f))
                except OSError:
                    pass
                open(f, 'w').write(files[f])

    def make_client_and_bzrdir(self, repospath, clientpath):
        repos_url = self.make_client(repospath, clientpath)

        return BzrDir.open("svn+%s" % repos_url)

    def make_client(self, repospath, clientpath, allow_revprop_changes=True):
        """Create a repository and a checkout. Return the checkout.

        :param relpath: Optional relpath to check out if not the full 
            repository.
        :param clientpath: Path to checkout
        :return: Repository URL.
        """
        repos_url = self.make_repository(repospath, 
            allow_revprop_changes=allow_revprop_changes)
        self.make_checkout(repos_url, clientpath)
        return repos_url

    def dumpfile(self, repos):
        """Create a dumpfile for the specified repository.

        :return: File name of the dumpfile.
        """
        raise NotImplementedError(self.dumpfile)

    def open_fs(self, relpath):
        """Open a fs.

        :return: FS.
        """
        repos = svn.repos.open(relpath)

        return svn.repos.fs(repos)


def test_suite():
    from unittest import TestSuite
    
    from bzrlib.tests import TestUtil

    loader = TestUtil.TestLoader()

    suite = TestSuite()

    testmod_names = [
            'test_branch', 
            'test_branchprops', 
            'test_checkout',
            'test_commit',
            'test_config',
            'test_convert',
            'test_errors',
            'test_fetch',
            'test_fileids', 
            'test_graph', 
            'test_logwalker',
            'test_mapping',
            'test_push',
            'test_ra',
            'test_radir',
            'test_repos', 
            'test_revids',
            'test_revspec',
            'test_scheme', 
            'test_svk',
            'test_transport',
            'test_tree',
            'test_upgrade',
            'test_wc',
            'test_workingtree',
            'test_blackbox']
    suite.addTest(loader.loadTestsFromModuleNames(["%s.%s" % (__name__, i) for i in testmod_names]))

    return suite
