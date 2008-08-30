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

from bzrlib import urlutils
from bzrlib.errors import NotBranchError

class RepositoryLayout(object):
    """Describes a repository layout."""

    def get_tag_path(self, name, project=""):
        """Return the path at which the tag with specified name should be found.

        :param name: Name of the tag. 
        :param project: Optional name of the project the tag is for. Can include slashes.
        :return: Path of the tag."
        """
        raise NotImplementedError

    def get_tag_name(self, path, project=""):
        """Determine the tag name from a tag path.

        :param path: Path inside the repository.
        """
        raise NotImplementedError

    def push_merged_revisions(self, project=""):
        """Determine whether or not right hand side (merged) revisions should be pushed.

        Defaults to False.
        
        :param project: Name of the project.
        """
        return False

    def get_branch_path(self, name, project=""):
        """Return the path at which the branch with specified name should be found.

        :param name: Name of the branch. 
        :param project: Optional name of the project the branch is for. Can include slashes.
        :return: Path of the branch.
        """
        raise NotImplementedError

    def parse(self, path):
        """Parse a path.

        :return: Tuple with type ('tag', 'branch'), project name, branch path and path 
            inside the branch
        """
        raise NotImplementedError

    def is_branch(self, path, project=None):
        """Check whether a specified path points at a branch."""
        try:
            (type, proj, bp, rp) = self.parse(path)
        except NotBranchError:
            return False
        if (type == "branch" and rp == "" and 
            (project is None or proj == project)):
            return True
        return False

    def is_tag(self, path, project=None):
        """Check whether a specified path points at a tag."""
        try:
            (type, proj, bp, rp) = self.parse(path)
        except NotBranchError:
            return False
        if (type == "tag" and rp == "" and
            (project is None or proj == project)):
            return True
        return False

    def is_branch_or_tag(self, path, project=None):
        return self.is_branch(path, project) or self.is_tag(path, project)

    def is_branch_or_tag_parent(self, path, project=None):
        return self.is_branch_parent(path, project) or self.is_tag_parent(path, project)

    def get_branches(self, revnum, project="", pb=None):
        """Retrieve a list of paths that refer to branches in a specific revision.

        :result: Iterator over tuples with (project, branch path)
        """
        raise NotImplementedError

    def get_tags(self, revnum, project="", pb=None):
        """Retrieve a list of paths that refer to tags in a specific revision.

        :result: Iterator over tuples with (project, branch path)
        """
        raise NotImplementedError


class ConfigBasedLayout(RepositoryLayout):

    def __init__(self, repository):
        self._config = repository.get_config()

    def get_tag_path(self, name, project=""):
        """Return the path at which the tag with specified name should be found.

        :param name: Name of the tag. 
        :param project: Optional name of the project the tag is for. Can include slashes.
        :return: Path of the tag."
        """
        return urlutils.join(project, "tags", name)

    def get_tag_name(self, path, project=""):
        """Determine the tag name from a tag path.

        :param path: Path inside the repository.
        """
        return urlutils.basename(path)

    def push_merged_revisions(self, project=""):
        """Determine whether or not right hand side (merged) revisions should be pushed.

        Defaults to False.
        
        :param project: Name of the project.
        """
        return self._config.get_push_merged_revisions()

    def get_branch_path(self, name, project=""):
        """Return the path at which the branch with specified name should be found.

        :param name: Name of the branch. 
        :param project: Optional name of the project the branch is for. Can include slashes.
        :return: Path of the branch.
        """
        return urlutils.join(project, "branches", name)

    def parse(self, path):
        """Parse a path.

        :return: Tuple with type ('tag', 'branch'), project name, branch path and path 
            inside the branch
        """
        parts = path.split("/")
        for i, p in enumerate(parts):
            if (i > 0 and parts[i-1] in ("branches", "tags")) or p == "trunk":
                if p == "tags":
                    t = "tag"
                    j = i-1
                elif p == "branches":
                    t = "branch"
                    j = i-1
                else:
                    t = "branch"
                    j = i
                return (t, "/".join(parts[:j-1]).strip("/"), "/".join(parts[:i]).strip("/"), "/".join(parts[i+1:]))
        raise InvalidSvnBranchPath(path, self)

    def get_branches(self, revnum, project="", pb=None):
        """Retrieve a list of paths that refer to branches in a specific revision.

        :result: Iterator over tuples with (project, branch path)
        """
        raise NotImplementedError

    def get_tags(self, revnum, project="", pb=None):
        """Retrieve a list of paths that refer to tags in a specific revision.

        :result: Iterator over tuples with (project, branch path)
        """
        raise NotImplementedError

    def __repr__(self):
        return "%s()" % self.__class__.__name__

