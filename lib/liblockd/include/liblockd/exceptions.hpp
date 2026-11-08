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
 * File       : exceptions.hpp
 * Author     : Brett Gmoser
 * Part Of    : liblockd, a client library for interfacing with lockd.
 * Purpose    : Categorized client errors with source locations for diagnostics.
 * Start Date : 03/26/2012
 *
 * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */


#if !defined(LOCKD_LIBLOCKD_EXCEPTIONS_HPP_INCLUDED_)
# define LOCKD_LIBLOCKD_EXCEPTIONS_HPP_INCLUDED_

# if defined(_MSC_VER) && (_MSC_VER >= 1020)
#  pragma once
#  pragma warning(default : 4265) // enable warning about virtual dtors
# endif

# include <stdexcept>
# include <string>
# include <utility>

namespace lld {

class exception : public std::runtime_error
{
    /* enumerations */
public:
    typedef enum
    {
        // Call of a feature that is as of now unimplemented.
        et_unimplemented = 0

                // A serious logical error.
                ,
        et_logic_error

                // Unable to connect to lockd.
                ,
        et_lockd_unavailable

                // Read error with lockd.
                ,
        et_read_error

                // Write error with lockd.
                ,
        et_write_error

                // An operation was attempted that requires a connection to lockd, but the connection was
                // not open.
                ,
        et_not_connected

                // Something unexpected happened, like a reply from the server that wasn't what we expected.
                ,
        et_unexpected

                // An unknown exception was caught. Also used as a generic error inside of lockd itself.
                ,
        et_unknown_exception
    } error_type_t;

    /* construction and destruction */
public:
    exception(
            const error_type_t& error_type,
            std::string file,
            const unsigned int line,
            std::string function,
            const std::string& message
    )
            : std::runtime_error(message.c_str())
            , _errorType(error_type)
            , _file(std::move(file))
            , _line(line)
            , _function(std::move(function))
    {
    }

    exception(
            const error_type_t& error_type,
            std::string file,
            const unsigned int line,
            const std::string& message
    )
            : std::runtime_error(message.c_str())
            , _errorType(error_type)
            , _file(std::move(file))
            , _line(line)
    {
    }

    exception(const error_type_t& error_type, const std::string& message)
            : std::runtime_error(message.c_str())
            , _errorType(error_type)
            , _line(0)
    {
    }

    virtual ~exception() override = default;

    /* public methods */
public:
    [[nodiscard]] const error_type_t& error_type() const { return _errorType; }
    [[nodiscard]] const std::string& file() const { return _file; }
    [[nodiscard]] const std::string& function() const { return _function; }
    [[nodiscard]] const unsigned int& line() const { return _line; }

    /* private data members */
private:
    error_type_t _errorType;
    std::string _file;
    unsigned int _line;
    std::string _function;
};

} // namespace lld

#endif // !defined(LOCKD_LIBLOCKD_EXCEPTIONS_HPP_INCLUDED_)
