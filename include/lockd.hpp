/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 * Codexterous - Software. Thought through.
 * http://www.codexterous.com 
 * 
 * Copyright (C) 2026 Brett Gmoser <lockd@codexterous.com>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy of this software
 * and associated documentation files (the "Software"), to deal in the Software without
 * restriction, including without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * - The above copyright notice and this permission notice shall be included in all copies or
 *   substantial portions of the Software.
 *
 * - THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING
 *   BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 *   NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM,
 *   DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 *   FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 * File       : lockd.hpp
 * Author     : Brett Gmoser
 * Part Of    : lockd, an interprocess locking daemon
 * Purpose    : Parse daemon options and coordinate startup and shutdown.
 * Start Date : 03/26/2012
 *
 * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

#if !defined(LOCKD_LOCKD_LOCKD_HPP_INCLUDED_)
# define LOCKD_LOCKD_LOCKD_HPP_INCLUDED_

# if defined(_MSC_VER) && (_MSC_VER >= 1020)
#  pragma once
# endif

# include <algorithm>
# include <chrono>

namespace lkd {

extern bool IS_FORKED;
inline constexpr std::chrono::seconds SHUTDOWN_GRACE_PERIOD(30);

// Larger worker pools showed poor throughput gains for their CPU cost. Limit automatic
// selection while preserving small hardware counts and explicit --threads overrides.
[[nodiscard]] constexpr unsigned int default_worker_count(const unsigned int hardware_threads)
{
    return std::clamp(hardware_threads, 1u, 8u);
}

typedef enum
{
    rr_report_running,
    rr_no_report_running
} report_running_t;

} // namespace lkd

#endif // !defined(LOCKD_LOCKD_LOCKD_HPP_INCLUDED_)
