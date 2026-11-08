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
 * File       : test_output.hpp
 * Author     : Brett Gmoser
 * Part Of    : lockd, an interprocess locking daemon
 * Purpose    : Capture test diagnostics without hiding unexpected failures.
 * Start Date : 10/24/2026
 *
 * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

#if !defined(LOCKD_TEST_TEST_OUTPUT_HPP_INCLUDED_)
# define LOCKD_TEST_TEST_OUTPUT_HPP_INCLUDED_

# if defined(_MSC_VER) && (_MSC_VER >= 1020)
#  pragma once
# endif

# include <ostream>
# include <sstream>
# include <streambuf>

# include <boost/test/tree/observer.hpp>

namespace ldt {

// Preserve Boost.Test's normal reporting and progress output. Daemon logs are replayed only
// for failed tests or verbose runs; intentional assertion failures remain counted by Boost.Test.
class test_output : public boost::unit_test::test_observer
{
    /* construction and destruction */
public:
    test_output();
    virtual ~test_output() override;

    /* discarded methods */
public:
    test_output(const test_output&) = delete;
    test_output& operator=(const test_output&) = delete;

    /* public methods */
public:
    void test_unit_start(const boost::unit_test::test_unit& test) override;
    void
    test_unit_finish(const boost::unit_test::test_unit& test, const unsigned long elapsed) override;
    int priority() override { return 1; }

    /* private methods */
private:
    void _restoreStreams();

    /* private data members */
private:
    std::ostringstream _daemonOutput;
    std::ostringstream _expectedOutput;
    std::streambuf* _daemonStream = nullptr;
    std::ostream* _testStream = nullptr;
};

} // namespace ldt

#endif // !defined(LOCKD_TEST_TEST_OUTPUT_HPP_INCLUDED_)
