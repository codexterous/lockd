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
 * File       : client_contract.cpp
 * Author     : Brett Gmoser
 * Part Of    : liblockd, a client library for interfacing with lockd.
 * Purpose    : Share client validation, reply framing, and timeout rules.
 * Start Date : 10/30/2026
 *
 * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

#include <chrono>
#include <cmath>
#include <ratio>
#include <string>

#include <boost/algorithm/string/classification.hpp>
#include <boost/algorithm/string/trim.hpp>

// The X3 umbrella initializes traits needed by individual operator headers in Boost 1.82.
#include <boost/spirit/home/x3.hpp> // IWYU pragma: keep
#include <boost/spirit/home/x3/auxiliary/eoi.hpp>
#include <boost/spirit/home/x3/core/parse.hpp>
#include <boost/spirit/home/x3/operator/sequence.hpp>

#include "../../include/liblockd/detail/client_contract.hpp"
#include "../../include/liblockd/detail/lock_name.hpp"
#include "../../include/liblockd/detail/source_path.hpp"
#include "../../include/liblockd/exceptions.hpp"

void lld::detail::validate_lock_name(const std::string& name)
{
    if (!boost::spirit::x3::parse(name.begin(), name.end(), lock_name >> boost::spirit::x3::eoi))
        throw lld::exception(
                lld::exception::et_logic_error,
                LOCKD_SOURCE_FILE,
                __LINE__,
                __FUNCTION__,
                "Lock name must contain 1-256 ASCII letters, digits, underscores or hyphens."
        );
}


void lld::detail::validate_connect_timeout(const std::chrono::seconds& timeout)
{
    const auto limit = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::steady_clock::time_point::max() - std::chrono::steady_clock::now()
    );
    if (timeout < std::chrono::seconds::zero() || timeout > limit)
        throw lld::exception(
                lld::exception::et_logic_error,
                LOCKD_SOURCE_FILE,
                __LINE__,
                __FUNCTION__,
                "Connection timeout must be nonnegative and fit the steady clock range."
        );
}


std::chrono::milliseconds lld::detail::lock_wait_milliseconds(const double milliseconds)
{
    constexpr double limit = std::chrono::duration<double, std::milli>(
                                     std::chrono::steady_clock::duration::max() / 2
    )
                                     .count();
    if (!std::isfinite(milliseconds) || milliseconds > limit)
        throw lld::exception(
                lld::exception::et_logic_error,
                LOCKD_SOURCE_FILE,
                __LINE__,
                __FUNCTION__,
                "Lock timeout must be finite and fit the system clock"
        );
    if (milliseconds <= 0)
        return std::chrono::milliseconds::zero();
    return std::chrono::ceil<std::chrono::milliseconds>(
            std::chrono::duration<double, std::milli>(milliseconds)
    );
}


void lld::detail::trim_reply(std::string& reply)
{
    boost::algorithm::trim_if(reply, boost::algorithm::is_any_of("\r\n\t "));
}
