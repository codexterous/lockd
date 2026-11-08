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
 * File       : program_options.hpp
 * Author     : Brett Gmoser
 * Part Of    : lockd_test, a unit test suite for lockd and liblockd.
 * Purpose    : Parse test-runner options and manage the protocol suite fixture.
 * Start Date : 08/03/2023
 *
 * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

#if !defined(LOCKD_LOCKD_TEST_PROGRAM_OPTIONS_HPP_INCLUDED_)
# define LOCKD_LOCKD_TEST_PROGRAM_OPTIONS_HPP_INCLUDED_

# if defined(_MSC_VER) && (_MSC_VER >= 1020)
#  pragma once
# endif

# include <cstdint>
# include <memory>
# include <optional>
# include <string>

# include "../include/launcher.hpp"

class program_options_fixture
{
    /* construction and destruction */
public:
    explicit program_options_fixture(const std::string& executable = "");
    virtual ~program_options_fixture();

    /* public methods */
public:
    // Return an exit status for help or invalid options; otherwise let Boost.Test run.
    [[nodiscard]] static std::optional<int>
    process_options(const int argc, const char* const* const argv);
    void setup();
    void teardown() const;
    [[nodiscard]] static bool invalid_options();

    /* public methods, observers */
public:
    [[nodiscard]] const std::string& interface() const { return _interface; }
    [[nodiscard]] const std::string& lockd_path() const { return _lockdPath; }
    [[nodiscard]] std::uint16_t port() const { return _port; }
    [[nodiscard]] unsigned short threads() const { return _threads; }

    [[nodiscard]] std::shared_ptr<const ldt::launcher> launcher() const { return _launcher; }
    [[nodiscard]] std::shared_ptr<ldt::launcher> launcher() { return _launcher; }

    /* private methods */
private:
    bool _parseOptions(const int argc, const char* const* const argv);

    /* private data members */
private:
    std::string _interface;
    std::string _lockdPath;
    std::uint16_t _port;
    unsigned short _threads;
    std::shared_ptr<ldt::launcher> _launcher;
};


extern program_options_fixture* PROGRAM_OPTIONS_FIXTURE_PTR;

#endif // !defined(LOCKD_LOCKD_TEST_PROGRAM_OPTIONS_HPP_INCLUDED_)
