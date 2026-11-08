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
 * File       : fixed_clock.hpp
 * Author     : Brett Gmoser
 * Part Of    : lockd, an interprocess locking daemon
 * Purpose    : Fixed clock for deterministic deadline conversion tests.
 * Start Date : 10/23/2026
 *
 * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

#if !defined(LOCKD_TEST_FIXED_CLOCK_HPP_INCLUDED_)
# define LOCKD_TEST_FIXED_CLOCK_HPP_INCLUDED_

# if defined(_MSC_VER) && (_MSC_VER >= 1020)
#  pragma once
# endif

# include <chrono>

namespace ldt {

// Keep now() at the epoch so sub-millisecond deadlines cannot expire during a test.
template <typename Duration> class fixed_clock
{
    /* typedefs */
public:
    typedef Duration duration;
    typedef typename duration::rep rep;
    typedef typename duration::period period;
    typedef std::chrono::time_point<fixed_clock> time_point;

    /* construction and destruction */
public:
    virtual ~fixed_clock() = default;

    /* public methods */
public:
    static time_point now() noexcept { return time_point(); }

    /* public constants */
public:
    static constexpr bool is_steady = true;
};

} // namespace ldt

#endif // !defined(LOCKD_TEST_FIXED_CLOCK_HPP_INCLUDED_)
