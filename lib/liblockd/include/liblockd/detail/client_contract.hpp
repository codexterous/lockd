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
 * File       : client_contract.hpp
 * Author     : Brett Gmoser
 * Part Of    : liblockd, a client library for interfacing with lockd.
 * Purpose    : Share client validation, reply framing, and timeout rules.
 * Start Date : 10/30/2026
 *
 * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

#if !defined(LOCKD_LIBLOCKD_DETAIL_CLIENT_CONTRACT_HPP_INCLUDED_)
# define LOCKD_LIBLOCKD_DETAIL_CLIENT_CONTRACT_HPP_INCLUDED_

# if defined(_MSC_VER) && (_MSC_VER >= 1020)
#  pragma once
# endif

# include <chrono>
# include <cstddef>
# include <string>

# include <liblockd/api.hpp>

namespace lld::detail {

// Both native and Python clients use these rules before opening a connection or sending a
// command. Positive waits round upward to the wire's millisecond resolution. Nonpositive
// waits make one TRY_LOCK attempt; nonfinite and unrepresentable waits are usage errors.
inline constexpr std::size_t reply_buffer_limit = 4096;
inline constexpr std::chrono::seconds greeting_timeout{10};
inline constexpr std::chrono::seconds command_timeout{30};
inline constexpr std::chrono::seconds timed_ack_timeout{10};
LIBLOCKD_API void validate_lock_name(const std::string& name);
LIBLOCKD_API void validate_connect_timeout(const std::chrono::seconds& timeout);
LIBLOCKD_API std::chrono::milliseconds lock_wait_milliseconds(const double milliseconds);
LIBLOCKD_API void trim_reply(std::string& reply);

} // namespace lld::detail

#endif // !defined(LOCKD_LIBLOCKD_DETAIL_CLIENT_CONTRACT_HPP_INCLUDED_)
