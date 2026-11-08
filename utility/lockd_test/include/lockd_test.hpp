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
 * File       : lockd_test.hpp
 * Author     : Brett Gmoser
 * Part Of    : lockd_test, a unit test suite for lockd and liblockd.
 * Purpose    : Shared test-runner entry points and suite coordination.
 * Start Date : 03/26/2012
 *
 * Description: The protocol suite shares daemon and client state, so its cases run together.
 *              Lifetime and network cases use isolated fixtures for buffer limits, disconnects,
 *              cancellation, and ownership handoff. C-interface cases check handles and string
 *              buffers; shared builds also load exports through the native platform loader.
 *              Platform cases cover POSIX startup and Windows service/elevation boundaries.
 *              Test-specific scenarios and assertions stay beside the individual cases.
 *
 * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

#if !defined(LOCKD_LOCKD_TEST_LOCKD_TEST_HPP_INCLUDED_)
# define LOCKD_LOCKD_TEST_LOCKD_TEST_HPP_INCLUDED_

# if defined(_MSC_VER) && (_MSC_VER >= 1020)
#  pragma once
# endif

# include <string>

namespace ldt {

// Run one contender in a separate process; return failure if its lock result is unexpected.
int child_process_contender(const int argc, const char* const argv[]);
int master_test_suite_argc();
char** master_test_suite_argv();
void release_protocol_clients();
void throw_framework_setup_error(const std::string& error_message);

//// We have a protected method on lld::mutex that we need to test, so, expose it here.
//class unprotected_mutex : public lld::mutex {};

} // namespace ldt

#endif // !defined(LOCKD_LOCKD_TEST_LOCKD_TEST_HPP_INCLUDED_)
