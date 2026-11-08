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
 * File       : process.hpp
 * Author     : Brett Gmoser
 * Part Of    : lockd, an interprocess locking daemon
 * Purpose    : Boost.Process v1 selection for test subprocesses.
 * Start Date : 10/24/2026
 *
 * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

#if !defined(LOCKD_TEST_PROCESS_HPP_INCLUDED_)
# define LOCKD_TEST_PROCESS_HPP_INCLUDED_

# if defined(_MSC_VER) && (_MSC_VER >= 1020)
#  pragma once
# endif

# include <boost/version.hpp>

// Boost 1.88 switched the unversioned header to Process v2. Keep the existing
// child, pipe, and redirection API on both sides of that change. The individual
// v1 headers also work in 1.88, which has no v1 umbrella header.
// This adapter intentionally exports the selected Process API to its includers.
// IWYU pragma: begin_exports
# if BOOST_VERSION >= 108800
#  include <boost/process/v1/args.hpp>
#  include <boost/process/v1/child.hpp>
#  include <boost/process/v1/env.hpp>
#  include <boost/process/v1/environment.hpp>
#  include <boost/process/v1/exception.hpp>
#  include <boost/process/v1/io.hpp>
#  include <boost/process/v1/pipe.hpp>
#  include <boost/process/v1/search_path.hpp>
#  include <boost/process/v1/start_dir.hpp>

# else
#  include <boost/process/args.hpp>
#  include <boost/process/child.hpp>
#  include <boost/process/env.hpp>
#  include <boost/process/environment.hpp>
#  include <boost/process/exception.hpp>
#  include <boost/process/io.hpp>
#  include <boost/process/pipe.hpp>
#  include <boost/process/search_path.hpp>
#  include <boost/process/start_dir.hpp>

# endif
// IWYU pragma: end_exports

namespace ldt {

# if BOOST_VERSION >= 108800
namespace process = boost::process::v1;
# else
namespace process = boost::process;
# endif

} // namespace ldt

#endif // !defined(LOCKD_TEST_PROCESS_HPP_INCLUDED_)
