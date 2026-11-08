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
 * File       : event_log.cpp
 * Author     : Brett Gmoser
 * Part Of    : lockd, an interprocess locking daemon
 * Purpose    : Windows service Event Log setup and filtering.
 * Start Date : 10/23/2026
 *
 * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

#ifdef WIN32

# include <string>

// Supplies the expression operators used by Boost.Log formatters and filters.
# include <boost/log/attributes/constant.hpp>
# include <boost/log/core/record_view.hpp>
# include <boost/log/expressions.hpp> // IWYU pragma: keep
# include <boost/log/expressions/attr.hpp>
# include <boost/log/expressions/filter.hpp>
# include <boost/log/expressions/formatter.hpp>
# include <boost/log/expressions/message.hpp>
# include <boost/log/keywords/log_name.hpp>
# include <boost/log/keywords/log_source.hpp>
# include <boost/log/keywords/message_file.hpp>
# include <boost/log/keywords/registration.hpp>
# include <boost/log/sinks/event_log_backend.hpp>
# include <boost/log/sinks/event_log_constants.hpp>
# include <boost/log/sinks/sync_frontend.hpp>
# include <boost/log/sources/record_ostream.hpp>
# include <boost/log/sources/severity_feature.hpp>
# include <boost/log/sources/severity_logger.hpp>
# include <boost/log/utility/formatting_ostream.hpp>
# include <boost/log/utility/formatting_ostream_fwd.hpp>

# include <boost/make_shared.hpp> // IWYU pragma: keep

// Windows SDK declarations require the public umbrella and its prerequisite definitions.
# include <windows.h> // IWYU pragma: keep

# include "../../../include/logging.hpp"
# include "../../../include/platform/win32/detail/unicode.hpp"
# include "../../../include/platform/win32/event_log.hpp"
# include "../../../include/platform/win32/service_elevation.hpp"
# include "event_log_messages.h"


boost::log::filter lkd::platform::win32::event_log_filter(const log_level_t level)
{
    namespace expr = boost::log::expressions;

    // Compare the value explicitly; a bare attribute actor tests extraction, even for false.
    return expr::attr<bool>("ServiceLifecycle").or_default(false) == true ||
           expr::attr<log_level_t>("Severity") >= level;
}


boost::log::sinks::event_log::event_type
lkd::platform::win32::event_log_type(const log_level_t level)
{
    namespace events = boost::log::sinks::event_log;
    if (level >= ll_error)
        return events::error;
    if (level == ll_warning)
        return events::warning;
    return events::info;
}


boost::log::sinks::event_log::wevent_composer lkd::platform::win32::event_log_composer()
{
    namespace events = boost::log::sinks::event_log;
    namespace expr = boost::log::expressions;
    events::wevent_composer composer([](const boost::log::record_view&) {
        return events::make_event_id(LOCKD_EVENT_MESSAGE);
    });

    // Register a wide formatter for the message insertion. Daemon text uses UTF-8,
    // while Event Viewer expects UTF-16 insertion strings.
    const boost::log::wformatter message = [](const boost::log::record_view& record,
                                              boost::log::wformatting_ostream& stream) {
        const auto value = record[expr::smessage];
        if (value)
            stream << detail::from_utf8(value.get(), true);
    };
    composer[LOCKD_EVENT_MESSAGE] % message;
    return composer;
}


void lkd::platform::win32::setup_event_log(const std::string& service_name, const log_level_t level)
{
    namespace sinks = boost::log::sinks;
    namespace keywords = boost::log::keywords;
    typedef sinks::synchronous_sink<sinks::wevent_log_backend> event_sink;

    // The message table is linked directly into lockd.exe in both static and shared builds.
    // Keep registration scoped to the service path, using a lockd-specific source name.
    const auto executable = service_elevation::executable_path();

    // Boost.Log owns sinks through boost::shared_ptr; this is a library interface requirement.
    const auto backend = boost::make_shared<sinks::wevent_log_backend>(
            keywords::message_file = executable,
            keywords::log_name = L"Application",
            keywords::log_source = detail::from_utf8("lockd." + service_name),
            keywords::registration = sinks::event_log::on_demand
    );
    backend->set_event_composer(event_log_composer());
    backend->set_event_type_mapper([](const boost::log::record_view& record) {
        const auto severity = record["Severity"].extract<log_level_t>();
        return event_log_type(severity.get());
    });
    const auto sink = boost::make_shared<event_sink>(backend);
    sink->set_filter(event_log_filter(level));
    boost::log::core::get()->add_sink(sink);
}


void lkd::platform::win32::service_event(const std::string& message)
{
    // Lifecycle events are Information records even when ordinary Information logging is off.
    // The attribute bypasses only the Event Log sink's threshold, not the file sink's filter.
    boost::log::sources::severity_logger<log_level_t> logger;
    logger.add_attribute("ServiceLifecycle", boost::log::attributes::constant<bool>(true));
    BOOST_LOG_SEV(logger, ll_info) << message;
}

#endif // WIN32
