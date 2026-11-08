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
 * File       : shared_library.hpp
 * Author     : Brett Gmoser
 * Part Of    : lockd, an interprocess locking daemon
 * Purpose    : Native runtime loading for shared-library export tests.
 * Start Date : 10/22/2026
 *
 * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */


#if !defined(LOCKD_TEST_SHARED_LIBRARY_HPP_INCLUDED_)
# define LOCKD_TEST_SHARED_LIBRARY_HPP_INCLUDED_

# if defined(_MSC_VER)
#  pragma once
# endif

# include <cstring>
# include <filesystem>

namespace ldt {

// Test-only ownership of a runtime-loaded library. All resolved functions and DLL-owned
// handles must stop being used before this object unloads the library.
class shared_library
{
    /* construction and destruction */
public:
    shared_library() = default;
    shared_library(const shared_library&) = delete;
    shared_library& operator=(const shared_library&) = delete;
    virtual ~shared_library();

    /* public methods */
public:
    void load(const std::filesystem::path& path);
    void unload();
    [[nodiscard]] bool is_loaded() const;
    [[nodiscard]] bool has(const char* const name) const;
    template <typename T> T& get(const char* const name) const
    {
        const auto address = _symbol(name);
        T* function;
        static_assert(
                sizeof function == sizeof address, "Loader and function pointer sizes differ"
        );

        // Native loaders return symbol addresses. Copy their representation to avoid compiler
        // diagnostics for casts between function signatures with different calling conventions.
        std::memcpy(&function, &address, sizeof function);
        return *function;
    }

    /* private methods */
private:
    void* _symbol(const char* const name) const;

    /* private data members */
private:
    void* _handle = nullptr;
};

} // namespace ldt

#endif // !defined(LOCKD_TEST_SHARED_LIBRARY_HPP_INCLUDED_)
