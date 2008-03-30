# Copyright (C) 2006-2008 Jelmer Vernooij <jelmer@samba.org>

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
"""Subversion repository access."""

import bzrlib
from bzrlib import osutils, ui, urlutils, xml5
from bzrlib.branch import Branch, BranchCheckResult
from bzrlib.errors import (InvalidRevisionId, NoSuchRevision, NotBranchError, 
                           UninitializableFormat, UnrelatedBranches)
from bzrlib.graph import CachingParentsProvider
from bzrlib.inventory import Inventory
from bzrlib.lockable_files import LockableFiles, TransportLock
from bzrlib.repository import Repository, RepositoryFormat
from bzrlib.revisiontree import RevisionTree
from bzrlib.revision import Revision, NULL_REVISION
from bzrlib.transport import Transport, get_transport
from bzrlib.trace import info, mutter

from core import SubversionException
import core
import constants

import os

from branchprops import PathPropertyProvider
from cache import create_cache_dir, sqlite3
from config import SvnRepositoryConfig
import errors
import logwalker
from mapping import (SVN_PROP_BZR_REVISION_ID, SVN_REVPROP_BZR_SIGNATURE,
                     SVN_PROP_BZR_BRANCHING_SCHEME, BzrSvnMappingv3FileProps,
                     parse_revision_metadata, parse_revid_property, 
                     parse_merge_property, BzrSvnMapping,
                     get_default_mapping, parse_revision_id)
from revids import CachingRevidMap, RevidMap
from scheme import (BranchingScheme, ListBranchingScheme, 
                    parse_list_scheme_text, guess_scheme_from_history)
from svk import (SVN_PROP_SVK_MERGE, svk_features_merged_since, 
                 parse_svk_feature)
from tree import SvnRevisionTree
import urllib

class lazy_dict(object):
    def __init__(self, create_fn, *args):
        self.create_fn = create_fn
        self.args = args
        self.dict = None

    def _ensure_init(self):
        if self.dict is None:
            self.dict = self.create_fn(*self.args)

    def __len__(self):
        self._ensure_init()
        return len(self.dict)

    def __getitem__(self, key):
        self._ensure_init()
        return self.dict.__getitem__(key)

    def __setitem__(self, key, value):
        self._ensure_init()
        return self.dict.__setitem__(key, value)

    def __contains__(self, key):
        self._ensure_init()
        return self.dict.__contains__(key)

    def get(self, key, default=None):
        self._ensure_init()
        return self.dict.get(key, default)

    def has_key(self, key):
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


def svk_feature_to_revision_id(feature, mapping):
    """Convert a SVK feature to a revision id for this repository.

    :param feature: SVK feature.
    :return: revision id.
    """
    try:
        (uuid, bp, revnum) = parse_svk_feature(feature)
    except errors.InvalidPropertyValue:
        return None
    if not mapping.is_branch(bp) and not mapping.is_tag(bp):
        return None
    return mapping.generate_revision_id(uuid, revnum, bp)


class SvnRepositoryFormat(RepositoryFormat):
    """Repository format for Subversion repositories (accessed using svn_ra).
    """
    rich_root_data = True

    def __get_matchingbzrdir(self):
        from remote import SvnRemoteFormat
        return SvnRemoteFormat()

    _matchingbzrdir = property(__get_matchingbzrdir)

    def __init__(self):
        super(SvnRepositoryFormat, self).__init__()

    def get_format_description(self):
        return "Subversion Repository"

    def initialize(self, url, shared=False, _internal=False):
        raise UninitializableFormat(self)

    def check_conversion_target(self, target_repo_format):
        return target_repo_format.rich_root_data


def changes_path(changes, path):
    """Check if one of the specified changes applies 
    to path or one of its children.
    """
    for p in changes:
        assert isinstance(p, str)
        if p == path or p.startswith(path+"/") or path == "":
            return True
    return False



CACHE_DB_VERSION = 3

cachedbs = {}

class SvnRepository(Repository):
    """
    Provides a simplified interface to a Subversion repository 
    by using the RA (remote access) API from subversion
    """
    def __init__(self, bzrdir, transport, branch_path=None):
        from bzrlib.plugins.svn import lazy_register_optimizers
        lazy_register_optimizers()
        from fileids import CachingFileIdMap, simple_apply_changes, FileIdMap
        _revision_store = None

        assert isinstance(transport, Transport)

        control_files = LockableFiles(transport, '', TransportLock)
        Repository.__init__(self, SvnRepositoryFormat(), bzrdir, 
            control_files, None, None, None)

        self.transport = transport
        self.uuid = transport.get_uuid()
        assert self.uuid is not None
        self.base = transport.base
        assert self.base is not None
        self._serializer = xml5.serializer_v5
        self.dir_cache = {}
        self.get_config().add_location(self.base)
        self._log = logwalker.LogWalker(transport=transport)
        self.fileid_map = FileIdMap(simple_apply_changes, self)
        self.revmap = RevidMap(self)
        self._scheme = None
        self._hinted_branch_path = branch_path

        cache = self.get_config().get_use_cache()

        if cache:
            cache_dir = self.create_cache_dir()
            cachedir_transport = get_transport(cache_dir)
            cache_file = os.path.join(cache_dir, 'cache-v%d' % CACHE_DB_VERSION)
            if not cachedbs.has_key(cache_file):
                cachedbs[cache_file] = sqlite3.connect(cache_file)
            self.cachedb = cachedbs[cache_file]
            self._log = logwalker.CachingLogWalker(self._log, cache_db=self.cachedb)
            self.fileid_map = CachingFileIdMap(cachedir_transport, self.fileid_map)
            self.revmap = CachingRevidMap(self.revmap, self.cachedb)

        self.branchprop_list = PathPropertyProvider(self._log)

    def get_revmap(self):
        return self.revmap

    def lhs_missing_revisions(self, revhistory, stop_revision):
        missing = []
        slice = revhistory[:revhistory.index(stop_revision)+1]
        for revid in reversed(slice):
            if self.has_revision(revid):
                missing.reverse()
                return missing
            missing.append(revid)
        raise UnrelatedBranches()
    
    def get_transaction(self):
        raise NotImplementedError(self.get_transaction)

    def get_stored_scheme(self):
        """Retrieve the stored branching scheme, either in the repository 
        or in the configuration file.
        """
        scheme = self.get_config().get_branching_scheme()
        if scheme is not None:
            return (scheme, self.get_config().branching_scheme_is_mandatory())

        last_revnum = self.transport.get_latest_revnum()
        scheme = self._get_property_scheme(last_revnum)
        if scheme is not None:
            return (scheme, True)

        return (None, False)

    def get_mapping(self):
        return get_default_mapping()(self.get_scheme())

    def _make_parents_provider(self):
        return CachingParentsProvider(self)

    def get_scheme(self):
        """Determine the branching scheme to use for this repository.

        :return: Branching scheme.
        """
        # First, try to use the branching scheme we already know
        if self._scheme is not None:
            return self._scheme

        (scheme, mandatory) = self.get_stored_scheme()
        if mandatory:
            self._scheme = scheme
            return scheme

        if scheme is not None:
            if (self._hinted_branch_path is None or 
                scheme.is_branch(self._hinted_branch_path)):
                self._scheme = scheme
                return scheme

        last_revnum = self.transport.get_latest_revnum()
        self.set_branching_scheme(
            self._guess_scheme(last_revnum, self._hinted_branch_path),
            store=(last_revnum > 20),
            mandatory=False)

        return self._scheme

    def _get_property_scheme(self, revnum=None):
        if revnum is None:
            revnum = self.transport.get_latest_revnum()
        text = self.branchprop_list.get_properties("", revnum).get(SVN_PROP_BZR_BRANCHING_SCHEME, None)
        if text is None:
            return None
        return ListBranchingScheme(parse_list_scheme_text(text))

    def set_property_scheme(self, scheme):
        def done(revmetadata):
            pass
        editor = self.transport.get_commit_editor(
                {constants.PROP_REVISION_LOG: "Updating branching scheme for Bazaar."},
                done, None, False)
        root = editor.open_root(-1)
        editor.change_dir_prop(root, SVN_PROP_BZR_BRANCHING_SCHEME, 
                "".join(map(lambda x: x+"\n", scheme.branch_list)).encode("utf-8"))
        editor.close_directory(root)
        editor.close()

    def _guess_scheme(self, last_revnum, branch_path=None):
        scheme = guess_scheme_from_history(
            self._log.iter_changes("", last_revnum), last_revnum, 
            branch_path)
        mutter("Guessed branching scheme: %r" % scheme)
        return scheme

    def set_branching_scheme(self, scheme, store=True, mandatory=False):
        self._scheme = scheme
        if store:
            self.get_config().set_branching_scheme(str(scheme), 
                                                   mandatory=mandatory)

    def _warn_if_deprecated(self):
        # This class isn't deprecated
        pass

    def __repr__(self):
        return '%s(%r)' % (self.__class__.__name__, 
                           self.base)

    def create_cache_dir(self):
        cache_dir = create_cache_dir()
        dir = os.path.join(cache_dir, self.uuid)
        if not os.path.exists(dir):
            info("Initialising Subversion metadata cache in %s" % dir)
            os.mkdir(dir)
        return dir

    def _check(self, revision_ids):
        return BranchCheckResult(self)

    def get_inventory(self, revision_id):
        assert revision_id != None
        return self.revision_tree(revision_id).inventory

    def get_fileid_map(self, revnum, path, mapping):
        return self.fileid_map.get_map(self.uuid, revnum, path, mapping)

    def transform_fileid_map(self, uuid, revnum, branch, changes, renames, 
                             mapping):
        return self.fileid_map.apply_changes(uuid, revnum, branch, changes, 
                                             renames, mapping)[0]

    def all_revision_ids(self, mapping=None):
        if mapping is None:
            mapping = self.get_mapping()
        revnum = self.transport.get_latest_revnum()

        for (_, paths, revnum) in self._log.iter_changes("", revnum):
            yielded_paths = set()
            for p in paths:
                try:
                    bp = mapping.scheme.unprefix(p)[0]
                    if not bp in yielded_paths:
                        if not paths.has_key(bp) or paths[bp][0] != 'D':
                            assert revnum > 0 or bp == ""
                            yield self.generate_revision_id(revnum, bp, mapping)
                        yielded_paths.add(bp)
                except NotBranchError:
                    pass

    def get_inventory_weave(self):
        """See Repository.get_inventory_weave()."""
        raise NotImplementedError(self.get_inventory_weave)

    def set_make_working_trees(self, new_value):
        """See Repository.set_make_working_trees()."""
        pass # FIXME: ignored, nowhere to store it... 

    def make_working_trees(self):
        """See Repository.make_working_trees().

        Always returns False, as working trees are never created inside 
        Subversion repositories.
        """
        return False

    def iter_reverse_revision_history(self, revision_id):
        """Iterate backwards through revision ids in the lefthand history

        :param revision_id: The revision id to start with.  All its lefthand
            ancestors will be traversed.
        """
        if revision_id in (None, NULL_REVISION):
            return
        (branch_path, revnum, mapping) = self.lookup_revision_id(revision_id)
        for (bp, changes, rev, svn_revprops, svn_fileprops) in self.iter_reverse_branch_changes(branch_path, revnum, mapping):
            yield self.generate_revision_id(rev, bp, mapping, svn_revprops, svn_fileprops)

    def get_ancestry(self, revision_id, topo_sorted=True):
        """See Repository.get_ancestry().
        """
        ancestry = []
        graph = self.get_graph()
        for rev, parents in graph.iter_ancestry([revision_id]):
            if rev == NULL_REVISION:
                rev = None
            ancestry.append(rev)
        ancestry.reverse()
        return ancestry

    def has_revision(self, revision_id):
        """See Repository.has_revision()."""
        if revision_id is None:
            return True

        try:
            (path, revnum, _) = self.lookup_revision_id(revision_id)
        except NoSuchRevision:
            return False

        try:
            return (core.NODE_DIR == self.transport.check_path(path, revnum))
        except SubversionException, (_, num):
            if num == constants.ERR_FS_NO_SUCH_REVISION:
                return False
            raise

    def revision_trees(self, revids):
        """See Repository.revision_trees()."""
        for revid in revids:
            yield self.revision_tree(revid)

    def revision_tree(self, revision_id):
        """See Repository.revision_tree()."""
        if revision_id is None:
            revision_id = NULL_REVISION

        if revision_id == NULL_REVISION:
            inventory = Inventory(root_id=None)
            inventory.revision_id = revision_id
            return RevisionTree(self, inventory, revision_id)

        return SvnRevisionTree(self, revision_id)

    def lhs_revision_parent(self, path, revnum, mapping):
        """Find the mainline parent of the specified revision.

        :param path: Path of the revision in Subversion
        :param revnum: Subversion revision number
        :param mapping: Mapping.
        :return: Revision id of the left-hand-side parent or None if 
                  this is the first revision
        """
        assert isinstance(path, str)
        assert isinstance(revnum, int)

        if not mapping.is_branch(path) and \
           not mapping.is_tag(path):
            raise NoSuchRevision(self, 
                    self.generate_revision_id(revnum, path, mapping))

        # Make sure the specified revision actually exists
        changes = self._log.get_revision_paths(revnum)
        if not changes_path(changes, path):
            # the specified revno should be changing the branch or 
            # otherwise it is invalid
            raise NoSuchRevision(self, 
                    self.generate_revision_id(revnum, path, mapping))

        while True:
            next = logwalker.changes_find_prev_location(changes, path, revnum)
            if next is None:
                break
            (path, revnum) = next
            changes = self._log.get_revision_paths(revnum)

            if changes_path(changes, path):
                revid = self.generate_revision_id(revnum, path, mapping)
                if not mapping.is_branch(path) and \
                   not mapping.is_tag(path):
                       return None
                return revid
        return None

    def get_parent_map(self, revids):
        parent_map = {}
        for revision_id in revids:
            if revision_id == NULL_REVISION:
                parent_map[revision_id] = ()
            else:
                try:
                    parents = self.revision_parents(revision_id)
                except NoSuchRevision:
                    pass
                else:
                    if len(parents) == 0:
                        parents = (NULL_REVISION,)
                    parent_map[revision_id] = parents
        return parent_map

    def _svk_merged_revisions(self, branch, revnum, mapping, 
                              current_fileprops, previous_fileprops):
        """Find out what SVK features were merged in a revision.

        """
        current = current_fileprops.get(SVN_PROP_SVK_MERGE, "")
        if current == "":
            return
        previous = previous_fileprops.get(SVN_PROP_SVK_MERGE, "")
        for feature in svk_features_merged_since(current, previous):
            revid = svk_feature_to_revision_id(feature, mapping)
            if revid is not None:
                yield revid

    def revision_parents(self, revision_id, svn_fileprops=None, svn_revprops=None):
        """See Repository.revision_parents()."""
        (branch, revnum, mapping) = self.lookup_revision_id(revision_id)
        mainline_parent = self.lhs_revision_parent(branch, revnum, mapping)
        if mainline_parent is None:
            return ()

        parent_ids = (mainline_parent,)

        if svn_fileprops is None:
            svn_fileprops = lazy_dict(self.branchprop_list.get_changed_properties, branch, revnum)

        if svn_revprops is None:
            svn_revprops = lazy_dict(self.transport.revprop_list, revnum)

        extra_rhs_parents = mapping.get_rhs_parents(branch, svn_revprops, svn_fileprops)
        parent_ids += extra_rhs_parents

        if extra_rhs_parents == ():
            (prev_path, prev_revnum) = self._log.get_previous(branch, revnum)
            if prev_path is None and prev_revnum == -1:
                previous = {}
            else:
                previous = lazy_dict(self.branchprop_list.get_properties, prev_path.encode("utf-8"), prev_revnum)
            parent_ids += tuple(self._svk_merged_revisions(branch, revnum, mapping, svn_fileprops, previous))

        return parent_ids

    def get_revision(self, revision_id, svn_revprops=None, svn_fileprops=None):
        """See Repository.get_revision."""
        if not revision_id or not isinstance(revision_id, str):
            raise InvalidRevisionId(revision_id=revision_id, branch=self)

        (path, revnum, mapping) = self.lookup_revision_id(revision_id)
        
        if svn_revprops is None:
            svn_revprops = lazy_dict(self.transport.revprop_list, revnum)
        if svn_fileprops is None:
            svn_fileprops = lazy_dict(self.branchprop_list.get_changed_properties, path, revnum)
        parent_ids = self.revision_parents(revision_id, svn_fileprops=svn_fileprops, svn_revprops=svn_revprops)

        rev = Revision(revision_id=revision_id, parent_ids=parent_ids,
                       inventory_sha1=None)

        mapping.import_revision(svn_revprops, svn_fileprops, rev)

        return rev

    def get_revisions(self, revision_ids):
        """See Repository.get_revisions()."""
        # TODO: More efficient implementation?
        return map(self.get_revision, revision_ids)

    def add_revision(self, rev_id, rev, inv=None, config=None):
        raise NotImplementedError(self.add_revision)

    def generate_revision_id(self, revnum, path, mapping, revprops=None, changed_fileprops=None):
        """Generate an unambiguous revision id. 
        
        :param revnum: Subversion revision number.
        :param path: Branch path.
        :param mapping: Mapping to use.

        :return: New revision id.
        """
        assert isinstance(path, str)
        assert isinstance(revnum, int)
        assert isinstance(mapping, BzrSvnMapping)

        if revprops is None:
            revprops = lazy_dict(self._log._get_transport().revprop_list, revnum)

        if changed_fileprops is None:
            changed_fileprops = lazy_dict(self.branchprop_list.get_changed_properties, path, revnum)

        return self.get_revmap().get_revision_id(revnum, path, mapping, revprops, changed_fileprops)

    def lookup_revision_id(self, revid, scheme=None):
        """Parse an existing Subversion-based revision id.

        :param revid: The revision id.
        :param scheme: Optional repository layout to use when searching for 
                       revisions
        :raises: NoSuchRevision
        :return: Tuple with branch path, revision number and mapping.
        """
        # If there is no entry in the map, walk over all branches:
        if scheme is None:
            scheme = self.get_scheme()
        return self.get_revmap().get_branch_revnum(revid, scheme)

    def get_inventory_xml(self, revision_id):
        """See Repository.get_inventory_xml()."""
        return self.serialise_inventory(self.get_inventory(revision_id))

    def get_inventory_sha1(self, revision_id):
        """Get the sha1 for the XML representation of an inventory.

        :param revision_id: Revision id of the inventory for which to return 
         the SHA1.
        :return: XML string
        """

        return osutils.sha_string(self.get_inventory_xml(revision_id))

    def get_revision_xml(self, revision_id):
        """Return the XML representation of a revision.

        :param revision_id: Revision for which to return the XML.
        :return: XML string
        """
        return self._serializer.write_revision_to_string(
            self.get_revision(revision_id))

    def iter_reverse_branch_changes(self, branch_path, revnum, mapping):
        """Return all the changes that happened in a branch 
        until branch_path,revnum. 

        :return: iterator that returns tuples with branch path, 
            changed paths, revision number, changed file properties and 
            revision properties.
        """
        assert isinstance(branch_path, str)
        assert mapping.is_branch(branch_path) or mapping.is_tag(branch_path), \
                "Mapping %r doesn't accept %s as branch or tag" % (mapping, branch_path)

        for (bp, paths, revnum) in self._log.iter_changes(branch_path, revnum):
            assert revnum > 0 or bp == ""
            assert mapping.is_branch(bp) or mapping.is_tag(bp), "%r is not a valid path" % bp

            if not bp in paths:
                svn_fileprops = {}
            else:
                svn_fileprops = lazy_dict(self.branchprop_list.get_changed_properties, bp, revnum)
            svn_revprops = lazy_dict(self.transport.revprop_list, revnum)

            if (paths.has_key(bp) and paths[bp][1] is not None and 
                not (mapping.is_branch(paths[bp][1]) or mapping.is_tag(paths[bp][1]))):
                # Make it look like the branch started here if the mapping 
                # doesn't support weird paths as branches
                for c in self._log.find_children(paths[bp][1], paths[bp][2]):
                    path = c.replace(paths[bp][1], bp+"/", 1).replace("//", "/")
                    paths[path] = ('A', None, -1)
                paths[bp] = ('A', None, -1)

                yield (bp, paths, revnum, svn_revprops, svn_fileprops)
                return
                     
            yield (bp, paths, revnum, svn_revprops, svn_fileprops)

    def get_config(self):
        return SvnRepositoryConfig(self.uuid)

    def has_signature_for_revision_id(self, revision_id):
        """Check whether a signature exists for a particular revision id.

        :param revision_id: Revision id for which the signatures should be looked up.
        :return: False, as no signatures are stored for revisions in Subversion 
            at the moment.
        """
        try:
            (path, revnum, mapping) = self.lookup_revision_id(revision_id)
        except NoSuchRevision:
            return False
        revprops = self.transport.revprop_list(revnum)
        return revprops.has_key(SVN_REVPROP_BZR_SIGNATURE)

    def get_signature_text(self, revision_id):
        """Return the signature text for a particular revision.

        :param revision_id: Id of the revision for which to return the 
                            signature.
        :raises NoSuchRevision: Always
        """
        (path, revnum, mapping) = self.lookup_revision_id(revision_id)
        revprops = self.transport.revprop_list(revnum)
        try:
            return revprops[SVN_REVPROP_BZR_SIGNATURE]
        except KeyError:
            raise NoSuchRevision(self, revision_id)

    def add_signature_text(self, revision_id, signature):
        (path, revnum, mapping) = self.lookup_revision_id(revision_id)
        self.transport.change_rev_prop(revnum, SVN_REVPROP_BZR_SIGNATURE, signature)

    def get_revision_graph(self, revision_id=None):
        """See Repository.get_revision_graph()."""
        graph = self.get_graph()

        if revision_id is None:
            revision_ids = self.all_revision_ids(self.get_mapping())
        else:
            revision_ids = [revision_id]

        ret = {}
        for (revid, parents) in graph.iter_ancestry(revision_ids):
            if revid == NULL_REVISION:
                continue
            if (NULL_REVISION,) == parents:
                ret[revid] = ()
            else:
                ret[revid] = parents

        if revision_id is not None and revision_id != NULL_REVISION and ret[revision_id] is None:
            raise NoSuchRevision(self, revision_id)

        return ret

    def find_branches(self, using=False, scheme=None):
        """Find branches underneath this repository.

        This will include branches inside other branches.

        :param using: If True, list only branches using this repository.
        """
        # All branches use this repository, so the using argument can be 
        # ignored.
        if scheme is None:
            scheme = self.get_scheme()

        existing_branches = [bp for (bp, revnum, _) in 
                filter(lambda (bp, rev, exists): exists,
                       self.find_branchpaths(scheme))]

        branches = []
        for bp in existing_branches:
            try:
                branches.append(Branch.open(urlutils.join(self.base, bp)))
            except NotBranchError: # Skip non-directories
                pass
        return branches

    def find_branchpaths(self, scheme, from_revnum=0, to_revnum=None):
        """Find all branch paths that were changed in the specified revision 
        range.

        :param revnum: Revision to search for branches.
        :return: iterator that returns tuples with (path, revision number, still exists). The revision number is the revision in which the branch last existed.
        """
        assert scheme is not None
        if to_revnum is None:
            to_revnum = self.transport.get_latest_revnum()

        created_branches = {}

        ret = []

        pb = ui.ui_factory.nested_progress_bar()
        try:
            for i in range(from_revnum, to_revnum+1):
                pb.update("finding branches", i, to_revnum+1)
                paths = self._log.get_revision_paths(i)
                for p in sorted(paths.keys()):
                    if scheme.is_branch(p) or scheme.is_tag(p):
                        if paths[p][0] in ('R', 'D') and p in created_branches:
                            del created_branches[p]
                            if paths[p][1]:
                                prev_path = paths[p][1]
                                prev_rev = paths[p][2]
                            else:
                                prev_path = p
                                prev_rev = self._log.find_latest_change(p, 
                                    i-1, include_parents=True, 
                                    include_children=True)
                            assert isinstance(prev_rev, int)
                            ret.append((prev_path, prev_rev, False))

                        if paths[p][0] in ('A', 'R'): 
                            created_branches[p] = i
                    elif scheme.is_branch_parent(p) or \
                            scheme.is_tag_parent(p):
                        if paths[p][0] in ('R', 'D'):
                            k = created_branches.keys()
                            for c in k:
                                if c.startswith(p+"/") and c in created_branches:
                                    del created_branches[c] 
                                    j = self._log.find_latest_change(c, i-1, 
                                            include_parents=True, 
                                            include_children=True)
                                    assert isinstance(j, int)
                                    ret.append((c, j, False))
                        if paths[p][0] in ('A', 'R'):
                            parents = [p]
                            while parents:
                                p = parents.pop()
                                try:
                                    for c in self.transport.get_dir(p, i)[0].keys():
                                        n = p+"/"+c
                                        if scheme.is_branch(n) or scheme.is_tag(n):
                                            created_branches[n] = i
                                        elif (scheme.is_branch_parent(n) or 
                                              scheme.is_tag_parent(n)):
                                            parents.append(n)
                                except SubversionException, (_, constants.ERR_FS_NOT_DIRECTORY):
                                    pass
        finally:
            pb.finished()

        pb = ui.ui_factory.nested_progress_bar()
        i = 0
        for p in created_branches:
            pb.update("determining branch last changes", 
                      i, len(created_branches))
            j = self._log.find_latest_change(p, to_revnum, 
                                             include_parents=True,
                                             include_children=True)
            if j is None:
                j = created_branches[p]
            assert isinstance(j, int)
            ret.append((p, j, True))
            i += 1
        pb.finished()

        return ret

    def is_shared(self):
        """Return True if this repository is flagged as a shared repository."""
        return True

    def get_physical_lock_status(self):
        return False

    def get_commit_builder(self, branch, parents, config, timestamp=None, 
                           timezone=None, committer=None, revprops=None, 
                           revision_id=None):
        from commit import SvnCommitBuilder
        return SvnCommitBuilder(self, branch, parents, config, timestamp, 
                timezone, committer, revprops, revision_id)



