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
 * File       : source_path_test.cpp
 * Author     : Brett Gmoser
 * Part Of    : lockd, an interprocess locking daemon
 * Purpose    : Verify compile-time source paths and exception metadata.
 * Start Date : 11/11/2026
 *
 * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

#include <filesystem>
#include <string>
#include <string_view>

#include <boost/test/unit_test.hpp> // IWYU pragma: keep
#include <boost/test/unit_test_suite.hpp>

#include <liblockd/detail/source_path.hpp>
#include <liblockd/exceptions.hpp>

#include "source_path_fixture.hpp"

namespace {

BOOST_AUTO_TEST_SUITE(lifetimes)

BOOST_AUTO_TEST_CASE(source_paths_preserve_relative_locations_and_hide_external_roots)
{
    // Both compiler-remapped spellings must retain directories. These assertions also
    // require the production helper to remain usable during C++17 constant evaluation.
    static_assert(
            std::string_view(
                    lld::detail::relative_source_path(
                            "./lib/liblockd/source/detail/client_contract.cpp", "/checkout/"
                    )
            ) == "lib/liblockd/source/detail/client_contract.cpp"
    );
    static_assert(
            std::string_view(
                    lld::detail::relative_source_path(
                            "lib/liblockd/source/detail/client_contract.cpp", "/checkout/"
                    )
            ) == "lib/liblockd/source/detail/client_contract.cpp"
    );
    static_assert(
            std::string_view(
                    lld::detail::relative_source_path(
                            "././source/../include/test.hpp", "/checkout/"
                    )
            ) == "source/../include/test.hpp"
    );
    static_assert(
            std::string_view(
                    lld::detail::relative_source_path(R"(.\nested\file.cpp)", "C:/checkout/")
            ) == R"(nested\file.cpp)"
    );

    // Absolute names outside the configured source tree and SDK callers without a
    // configured root retain only their basename, including Windows and UNC paths.
    static_assert(
            std::string_view(
                    lld::detail::relative_source_path("/outside/include/test.hpp", "/checkout/")
            ) == "test.hpp"
    );
    static_assert(
            std::string_view(
                    lld::detail::relative_source_path("C:/checkout-other/test.hpp", "C:/checkout/")
            ) == "test.hpp"
    );
    static_assert(
            std::string_view(
                    lld::detail::relative_source_path(R"(\\server\share\test.hpp)", "C:/checkout/")
            ) == "test.hpp"
    );
    static_assert(
            std::string_view(
                    lld::detail::relative_source_path(R"(\outside\test.hpp)", "C:/checkout/")
            ) == "test.hpp"
    );
    static_assert(
            std::string_view(
                    lld::detail::relative_source_path("./consumer/include/test.hpp", "")
            ) == "test.hpp"
    );
    static_assert(
            std::string_view(
                    lld::detail::relative_source_path(
                            "C:/checkout/include/test.hpp", "C:/checkout/"
                    )
            ) == "include/test.hpp"
    );
    static_assert(
            std::string_view(
                    lld::detail::relative_source_path(
                            "C:/checkout/source/../include/test.hpp", "C:/checkout/"
                    )
            ) == "source/../include/test.hpp"
    );

    // Exercise the real throw-site macro with the current compiler flags as well as
    // the explicit input forms above. Compiler prefix-map flags must pass this check.
    BOOST_CHECK_EQUAL(
            std::filesystem::path(LOCKD_SOURCE_FILE).generic_string(),
            "utility/lockd_test/source/source_path_test.cpp"
    );
}

BOOST_AUTO_TEST_CASE(source_paths_follow_the_throwing_header_across_translation_units)
{
    const auto portable = [](const char* const name) {
        return std::filesystem::path(name).generic_string();
    };
    constexpr const char* header = "utility/lockd_test/include/source_path_fixture.hpp";
    BOOST_CHECK_EQUAL(portable(ldt::source_path_from_header()), header);
    BOOST_CHECK_EQUAL(portable(ldt::header_path_from_other_translation_unit()), header);
    BOOST_CHECK_EQUAL(
            portable(ldt::source_path_from_other_translation_unit()),
            "utility/lockd_test/source/source_path_fixture.cpp"
    );

    // An empty path fails the check if a fixture stops throwing. Keep assertions after
    // the catch, since a failure assertion after the inline throw would be unreachable.
    std::string header_exception_file;
    try {
        ldt::throw_from_source_path_header<int>();
    }
    catch (const lld::exception& error) {
        header_exception_file = portable(error.file().c_str());
    }
    BOOST_CHECK_EQUAL(header_exception_file, header);

    std::string other_exception_file;
    try {
        ldt::throw_from_other_translation_unit();
    }
    catch (const lld::exception& error) {
        other_exception_file = portable(error.file().c_str());
    }
    BOOST_CHECK_EQUAL(other_exception_file, header);
}

BOOST_AUTO_TEST_SUITE_END()

} // namespace
