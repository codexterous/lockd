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
 * File       : event_log.hpp
 * Author     : Brett Gmoser
 * Part Of    : lockd, an interprocess locking daemon
 * Purpose    : Windows service Event Log setup and filtering.
 * Start Date : 10/23/2026
 *
 * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

#if !defined(LOCKD_EVENT_LOG_HPP_INCLUDED_)
# define LOCKD_EVENT_LOG_HPP_INCLUDED_

# if defined(_MSC_VER) && (_MSC_VER >= 1020)
#  pragma once
# endif

# ifdef WIN32

#  include <string>

#  include <boost/log/expressions/filter.hpp>
#  include <boost/log/sinks/event_log_backend.hpp>
#  include <boost/log/sinks/event_log_constants.hpp>

#  include "../../logging.hpp"

namespace lkd::platform::win32 {

void setup_event_log(const std::string& service_name, const log_level_t level);
void service_event(const std::string& message);
[[nodiscard]] boost::log::sinks::event_log::wevent_composer event_log_composer();
[[nodiscard]] boost::log::filter event_log_filter(const log_level_t level);
[[nodiscard]] boost::log::sinks::event_log::event_type event_log_type(const log_level_t level);

} // namespace lkd::platform::win32

# endif // WIN32

#endif // !defined(LOCKD_EVENT_LOG_HPP_INCLUDED_)
