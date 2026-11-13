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
 * File       : source_path.hpp
 * Author     : Brett Gmoser
 * Part Of    : liblockd, a client library for interfacing with lockd.
 * Purpose    : Build source filenames at compile time for exception metadata.
 * Start Date : 11/08/2026
 *
 * Description: I really want exceptions to include the source file name, line, and function from
 *              where they were thrown, but I also don't want them to include path information from
 *              the machine that built the library or daemon.
 *
 *              CMake passes ${PROJECT_SOURCE_DIR} as LOCKD_SOURCE_ROOT, and a compile-time
 *              expression subtracts that root from each source file name, ultimately making the
 *              exception contain a path relative to the source root.
 *
 *              This has been tested and verified to evaluate at compile-time.
 *
 *              Use LOCKD_SOURCE_FILE everywhere you want to reference a project source file name.
 *
 * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

#if !defined(LOCKD_LIBLOCKD_DETAIL_SOURCE_PATH_HPP_INCLUDED_)
# define LOCKD_LIBLOCKD_DETAIL_SOURCE_PATH_HPP_INCLUDED_

# if defined(_MSC_VER) && (_MSC_VER >= 1020)
#  pragma once
# endif

# include <cstddef>
# include <string_view>

namespace lld::detail {

// CMake supplies the checkout prefix with a trailing slash. Preserve relative components
// within the remaining filename, whether CMake or the compiler removed the source root.
constexpr const char* relative_source_path(const char* const file, const std::string_view& root)
{
    const std::string_view path(file);
    std::size_t prefix = 0;
    while (prefix < root.size() && prefix < path.size() &&
           (path[prefix] == root[prefix] || (path[prefix] == '\\' && root[prefix] == '/')))
        ++prefix;
    if (!root.empty() && prefix == root.size())
        return file + prefix;

    // Reproducible builds can map __FILE__ to a relative path while the root definition
    // remains absolute. Keep that path in project builds, trimming only leading ./ markers.
    // Check both Unix and Windows roots here because std::filesystem is not constexpr in C++17.
    const bool qualified = !path.empty() && (path.front() == '/' || path.front() == '\\' ||
                                             (path.size() > 1 && path[1] == ':'));
    if (!root.empty() && !qualified) {
        std::size_t start = 0;
        while (path.size() - start >= 2 && path[start] == '.' &&
               (path[start + 1] == '/' || path[start + 1] == '\\'))
            start += 2;
        return file + start;
    }

    // An installed SDK has no build-machine root definition. Keep only the filename
    // for code outside the configured source tree, without exposing its absolute path.
    const auto separator = path.find_last_of("/\\");
    return file + (separator == std::string_view::npos ? 0 : separator + 1);
}

} // namespace lld::detail

# if !defined(LOCKD_SOURCE_ROOT)
#  define LOCKD_SOURCE_ROOT ""
# endif

// __FILE__ expands at the throw, including in headers. The constexpr initializer makes
// evaluation mandatory even in debug builds; each throw only retrieves a stored pointer.
# define LOCKD_SOURCE_FILE                                                                         \
     ([]() {                                                                                       \
      static constexpr const char* file =                                                          \
              ::lld::detail::relative_source_path(__FILE__, LOCKD_SOURCE_ROOT);                    \
      return file;                                                                                 \
     }())

#endif // !defined(LOCKD_LIBLOCKD_DETAIL_SOURCE_PATH_HPP_INCLUDED_)
