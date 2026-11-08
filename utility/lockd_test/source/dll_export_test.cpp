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
 * File       : dll_export_test.cpp
 * Author     : Brett Gmoser
 * Part Of    : lockd, an interprocess locking daemon
 * Purpose    : Verify shared-library exports through native runtime loading.
 * Start Date : 10/22/2026
 *
 * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */


#define BOOST_TEST_MODULE lockd_shared_exports
#define BOOST_TEST_MAIN
#define BOOST_TEST_NO_MAIN

#include <filesystem>
#include <initializer_list>
#include <memory>
#include <stdexcept>
#include <string>

// Initialize Winsock2 before Boost.Test can include windows.h.
#include <boost/asio.hpp> // IWYU pragma: keep
#include <boost/test/tools/context.hpp>
#include <boost/test/tools/interface.hpp> // IWYU pragma: keep
#include <boost/test/unit_test_suite.hpp>

// Provides the header-only Boost.Test framework implementation and initialization.
#include <boost/test/included/unit_test.hpp> // IWYU pragma: keep

#include <liblockd_dll/liblockd_dll.h>

#include "../include/daemon_fixture.hpp"
#include "../include/shared_library.hpp"
#include "../include/test_output.hpp"

namespace {

BOOST_AUTO_TEST_CASE(missing_library_and_symbol_report_loader_errors)
{
    // Try unloaded, missing-library, and missing-symbol lookups before using a valid export.
    // Verify errors leave the loader recoverable and the real library remains usable.
    ldt::shared_library library;
    BOOST_CHECK_MESSAGE(!library.has("liblockd_version"), "An unloaded library exposes no symbols");
    BOOST_CHECK_THROW(
            library.get<decltype(::liblockd_version)>("liblockd_version"), std::runtime_error
    );
    BOOST_CHECK_THROW(
            library.load(std::filesystem::path(LOCKD_C_LIBRARY_PATH).string() + ".missing"),
            std::runtime_error
    );
    BOOST_CHECK_MESSAGE(!library.is_loaded(), "A failed load leaves no usable library");

    // A failed load must not prevent loading the real artifact afterward.
    library.load(LOCKD_C_LIBRARY_PATH);
    BOOST_REQUIRE(library.is_loaded());
    BOOST_CHECK(!library.has("lockd_nonexistent_export"));
    BOOST_CHECK_THROW(
            library.get<decltype(::liblockd_version)>("lockd_nonexistent_export"),
            std::runtime_error
    );
    char version[1024];
    BOOST_CHECK(
            library.get<decltype(::liblockd_version)>("liblockd_version")(version, sizeof version)
    );
    BOOST_CHECK(std::string(version).find("liblockd_dll ") == 0);
}


BOOST_FIXTURE_TEST_CASE(
        c_exports_load_by_name_and_preserve_handle_ownership, ldt::running_server_fixture
)
{
    // Resolve C exports by name and repeat load/use/unload cycles against a real daemon. Verify
    // contention and destruction release handles before their library is unloaded.
    const auto port = lkd::detail::lifetime_test_access::endpoint(server).port();
    for (const auto cycle : {1, 2}) {
        BOOST_TEST_CONTEXT("load/use/unload cycle " << cycle)
        {
            // Declare the library first so every handle and its DLL-owned deleter dies before
            // unloading. decltype uses the public C signatures without linking to their symbols.
            ldt::shared_library library;
            library.load(LOCKD_C_LIBRARY_PATH);
            const auto create_mutex_fn = &library.get<decltype(::create_mutex)>("create_mutex");
            const auto destroy_mutex_fn = &library.get<decltype(::destroy_mutex)>("destroy_mutex");
            const auto create_lock_fn = &library.get<decltype(::create_lock)>("create_lock");
            const auto destroy_lock_fn = &library.get<decltype(::destroy_lock)>("destroy_lock");
            const auto lock_fn = &library.get<decltype(::lock)>("lock");
            const auto try_lock_fn = &library.get<decltype(::try_lock)>("try_lock");
            const auto unlock_fn = &library.get<decltype(::unlock)>("unlock");
            const auto error_fn = &library.get<decltype(::get_last_error)>("get_last_error");
            const auto version_fn = &library.get<decltype(::liblockd_version)>("liblockd_version");

            char version[1024];
            BOOST_REQUIRE(version_fn(version, sizeof version));
            BOOST_CHECK(std::string(version).find("liblockd_dll ") == 0);
            BOOST_CHECK(!version_fn(nullptr, 0));
            char truncated[2] = {'#', '#'};
            BOOST_CHECK(!version_fn(truncated, sizeof truncated));
            BOOST_CHECK_EQUAL(truncated[1], '\0');

            std::unique_ptr<void, decltype(&::destroy_mutex)> first(
                    create_mutex_fn("127.0.0.1", port, 1, "runtime-exports"), destroy_mutex_fn
            );
            std::unique_ptr<void, decltype(&::destroy_mutex)> second(
                    create_mutex_fn("127.0.0.1", port, 1, "runtime-exports"), destroy_mutex_fn
            );
            BOOST_REQUIRE_MESSAGE(first && second, "Resolved exports connect both mutex handles");
            std::unique_ptr<void, decltype(&::destroy_lock)> owner(
                    create_lock_fn(first.get()), destroy_lock_fn
            );
            std::unique_ptr<void, decltype(&::destroy_lock)> waiter(
                    create_lock_fn(second.get()), destroy_lock_fn
            );
            BOOST_REQUIRE(owner && waiter);
            BOOST_REQUIRE(lock_fn(owner.get()));
            BOOST_CHECK_EQUAL(try_lock_fn(waiter.get()), 1);
            BOOST_CHECK_MESSAGE(!lock_fn(owner.get()), "Repeated lock returns a C error result");
            char error[1024];
            BOOST_REQUIRE(error_fn(error, sizeof error));
            BOOST_CHECK_MESSAGE(error[0] != '\0', "Resolved error export reports the failure");
            BOOST_REQUIRE(unlock_fn(owner.get()));
            BOOST_REQUIRE_EQUAL(try_lock_fn(waiter.get()), 0);

            // Destroying the owning guard must release its remote lock through the loaded DLL.
            waiter.reset();
            BOOST_REQUIRE_EQUAL(try_lock_fn(owner.get()), 0);
            owner.reset();
            second.reset();
            first.reset();
            ldt::poll_until(server.io_service(), [&] { return server.client_count() == 0; });
            library.unload();
            BOOST_CHECK(!library.is_loaded());
        }
    }
}


} // namespace

int main(const int argc, char** const argv)
{
    const ldt::test_output output;
    return boost::unit_test::unit_test_main(&init_unit_test_suite, argc, argv);
}
