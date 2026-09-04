/*
obs-multireplay — a byte-counted cap, for a download that never declares its size
Copyright (C) 2026 obs-multireplay contributors
SPDX-License-Identifier: GPL-2.0-or-later

CURLOPT_MAXFILESIZE_LARGE only ever looks at a DECLARED Content-Length. A
chunked HTTP response has none, so a server (or a proxy, or a redirect) that
answers with Transfer-Encoding: chunked and an endless stream was written to
disk for the whole of kDownloadTimeoutSec — onto the disk a recording depends
on. The guard has to act on bytes actually written, which means it has to run
inside the libcurl write callback, which means it cannot be exercised by a
unit test without standing up a server. This is the one arithmetic fact that
callback rests on, pulled out so it can be tested without one.
*/

#pragma once

#include <cstddef>
#include <cstdint>

namespace multireplay {

namespace size_guard {

// True when writing `incoming` more bytes, after `writtenSoFar`, would cross
// `cap`. Exactly at the cap is still allowed — the caller decides whether to
// write this chunk and then stop, or to refuse it outright.
inline bool wouldOverflow(int64_t writtenSoFar, size_t incoming, int64_t cap)
{
	return writtenSoFar + (int64_t)incoming > cap;
}

} // namespace size_guard

} // namespace multireplay
