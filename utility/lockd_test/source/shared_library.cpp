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
 * File       : shared_library.cpp
 * Author     : Brett Gmoser
 * Part Of    : lockd, an interprocess locking daemon
 * Purpose    : Native runtime loading for shared-library export tests.
 * Start Date : 10/22/2026
 *
 * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */


#include <filesystem>
#include <stdexcept>
#include <string>

#if defined(WIN32)
# include <cstring>
# include <system_error>

// Windows SDK declarations require the public umbrella and its prerequisite definitions.
# include <windows.h> // IWYU pragma: keep
#else
# include <dlfcn.h>

#endif

#include "../include/shared_library.hpp"

ldt::shared_library::~shared_library()
{
    unload();
}


void ldt::shared_library::load(const std::filesystem::path& path)
{
    unload();
#if defined(WIN32)
    _handle = LoadLibraryW(path.c_str());
    if (!_handle)
        throw std::runtime_error(
                "Cannot load " + path.string() + ": " +
                std::system_category().message(static_cast<int>(GetLastError()))
        );
#else
    _handle = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!_handle)
        throw std::runtime_error("Cannot load " + path.string() + ": " + dlerror());
#endif
}


void ldt::shared_library::unload()
{
    if (!_handle)
        return;
#if defined(WIN32)
    FreeLibrary(static_cast<HMODULE>(_handle));
#else
    dlclose(_handle);
#endif
    _handle = nullptr;
}


bool ldt::shared_library::is_loaded() const
{
    return nullptr != _handle;
}


bool ldt::shared_library::has(const char* const name) const
{
    if (!_handle)
        return false;
#if defined(WIN32)
    return nullptr != GetProcAddress(static_cast<HMODULE>(_handle), name);
#else
    dlerror();

    // NOLINTNEXTLINE(bugprone-unused-return-value)
    static_cast<void>(dlsym(_handle, name));
    return nullptr == dlerror();
#endif
}


void* ldt::shared_library::_symbol(const char* const name) const
{
    if (!_handle)
        throw std::runtime_error("Cannot resolve a symbol from an unloaded library");
#if defined(WIN32)
    const auto symbol = GetProcAddress(static_cast<HMODULE>(_handle), name);
    if (!symbol)
        throw std::runtime_error(
                "Cannot resolve " + std::string(name) + ": " +
                std::system_category().message(static_cast<int>(GetLastError()))
        );
    void* address;
    static_assert(sizeof address == sizeof symbol, "Native symbol pointer sizes differ");
    std::memcpy(&address, &symbol, sizeof address);
    return address;
#else
    dlerror();
    void* const address = dlsym(_handle, name);
    const char* const error = dlerror();
    if (error)
        throw std::runtime_error("Cannot resolve " + std::string(name) + ": " + error);
    return address;
#endif
}
