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
 * File       : main.cpp
 * Author     : Brett Gmoser
 * Part Of    : lockd, an interprocess locking daemon
 * Purpose    : Exercise source paths across native compilation boundaries.
 * Start Date : 11/11/2026
 *
 * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

#include <filesystem>
#include <iostream>
#include <string>

#include <liblockd/detail/source_path.hpp>
#include <liblockd/exceptions.hpp>

#include "include/source_path_fixture.hpp"

namespace {

bool check_path(const char* const path, const std::string& expected)
{
    const auto actual = std::filesystem::path(path).generic_string();
    if (actual == expected)
        return true;
    std::cerr << "Expected source path '" << expected << "', received '" << actual << "'\n";
    return false;
}

} // namespace

int main()
{
    if (!check_path(ldt::source_path_from_other_translation_unit(), "nested/extra.cpp") ||
        !check_path(LOCKD_SOURCE_FILE, "main.cpp") ||
        !check_path(ldt::source_path_from_header(), "include/source_path_fixture.hpp") ||
        !check_path(
                ldt::header_path_from_other_translation_unit(), "include/source_path_fixture.hpp"
        ))
        return 1;
    // Keep failure checks outside the throwing calls, including when they are inlined.
    // An empty path also detects a fixture that unexpectedly returns without throwing.
    std::string header_exception_file;
    try {
        ldt::throw_from_source_path_header<int>();
    }
    catch (const lld::exception& error) {
        header_exception_file = error.file();
    }
    if (header_exception_file.empty())
        return 5;
    if (!check_path(header_exception_file.c_str(), "include/source_path_fixture.hpp"))
        return 2;

    std::string other_exception_file;
    try {
        ldt::throw_from_other_translation_unit();
    }
    catch (const lld::exception& error) {
        other_exception_file = error.file();
    }
    if (other_exception_file.empty())
        return 4;
    return check_path(other_exception_file.c_str(), "include/source_path_fixture.hpp") ? 0 : 3;
}
