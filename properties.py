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

def is_valid_property_name(prop):
    if not prop[0].isalnum() and not prop[0] in ":_":
        return False
    for c in prop[1:]:
        if not c.isalnum() and not c in "-:._":
            return False
    return True

def time_to_cstring(timestamp):
    import time
    tm_usec = timestamp % 1000000
    (tm_year, tm_mon, tm_mday, tm_hour, tm_min, 
            tm_sec, tm_wday, tm_yday, tm_isdst) = time.gmtime(timestamp / 1000000)
    return "%04d-%02d-%02dT%02d:%02d:%02d.%06dZ" % (tm_year, tm_mon, tm_mday, tm_hour, tm_min, tm_sec, tm_usec)

def time_from_cstring(text):
    import time
    (basestr, usecstr) = text.split(".", 1)
    assert usecstr[-1] == "Z"
    tm_usec = int(usecstr[:-1])
    tm = time.strptime(basestr, "%Y-%m-%dT%H:%M:%S")
    return (long(time.mktime((tm[0], tm[1], tm[2], tm[3], tm[4], tm[5], tm[6], tm[7], -1)) - time.timezone) * 1000000 + tm_usec)


PROP_EXECUTABLE = 'svn:executable'
PROP_EXECUTABLE_VALUE = '*'
PROP_EXTERNALS = 'svn:externals'
PROP_IGNORE = 'svn:ignore'
PROP_KEYWORDS = 'svn:keywords'
PROP_MIME_TYPE = 'svn:mime-type'
PROP_NEEDS_LOCK = 'svn:needs-lock'
PROP_NEEDS_LOCK_VALUE = '*'
PROP_PREFIX = 'svn:'
PROP_SPECIAL = 'svn:special'
PROP_SPECIAL_VALUE = '*'
PROP_WC_PREFIX = 'svn:wc:'
PROP_ENTRY_COMMITTED_DATE = 'svn:entry:committed-date'
PROP_ENTRY_COMMITTED_REV = 'svn:entry:committed-rev'
PROP_ENTRY_LAST_AUTHOR = 'svn:entry:last-author'
PROP_ENTRY_LOCK_TOKEN = 'svn:entry:lock-token'
PROP_ENTRY_UUID = 'svn:entry:uuid'

PROP_REVISION_LOG = "svn:log"
PROP_REVISION_AUTHOR = "svn:author"
PROP_REVISION_DATE = "svn:date"
