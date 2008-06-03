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

"""Subversion core library tests."""

from bzrlib.tests import TestCase
import core

class TestCore(TestCase):
    def setUp(self):
        super(TestCore, self).setUp()

    def test_exc(self):
        self.assertIsInstance(core.SubversionException("foo", 1), Exception)

    def test_get_config(self):
        self.assertIsInstance(core.get_config(), dict)

    def test_time_from_cstring(self):
        self.assertEquals(1225704780716938L, core.time_from_cstring("2008-11-03T09:33:00.716938Z"))

    def test_time_to_cstring(self):
        self.assertEquals("2008-11-03T09:33:00.716938Z", core.time_to_cstring(1225704780716938L))

