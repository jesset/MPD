/*
 * Copyright 2003-2018 The Music Player Daemon Project
 * http://www.musicpd.org
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
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#include "ArchiveInputPlugin.hxx"
#include "archive/ArchiveLookup.hxx"
#include "archive/ArchiveList.hxx"
#include "archive/ArchivePlugin.hxx"
#include "archive/ArchiveFile.hxx"
#include "../InputStream.hxx"
#include "fs/Path.hxx"
#include "Log.hxx"
#include "util/ScopeExit.hxx"

#include <string.h>

InputStreamPtr
OpenArchiveInputStream(Path path, Mutex &mutex)
{
	const ArchivePlugin *arplug;

	char *pname = strdup(path.c_str());
	AtScopeExit(pname) {
		free(pname);
	};

	// archive_lookup will modify pname when true is returned
	ArchiveLookupResult l;
	try {
		l = archive_lookup(pname);
		if (l.archive.IsNull()) {
			return nullptr;
		}
	} catch (...) {
		LogFormat(LogLevel::DEBUG, std::current_exception(),
			  "not an archive, lookup %s failed", pname);
		return nullptr;
	}

	const char *suffix = l.archive.GetSuffix();
	if (suffix == nullptr)
		return nullptr;

	//check which archive plugin to use (by ext)
	arplug = archive_plugin_from_suffix(suffix);
	if (!arplug) {
		return nullptr;
	}

	return archive_file_open(arplug, l.archive)
		->OpenStream(l.inside.c_str(), mutex);
}
