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
 * File       : api.hpp
 * Author     : Brett Gmoser
 * Part Of    : liblockd, a client library for interfacing with lockd.
 * Purpose    : Import and export declarations for the C++ client library.
 * Start Date : 10/20/2026
 *
 * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

#if !defined(LOCKD_LIBLOCKD_API_HPP_INCLUDED_)
# define LOCKD_LIBLOCKD_API_HPP_INCLUDED_

# if defined(_MSC_VER) && (_MSC_VER >= 1020)
#  pragma once
# endif

// Static consumers need no import declarations. The CMake target propagates LIBLOCKD_SHARED
// for shared builds; clients which link manually must define it themselves.
// Export out-of-line methods individually so we don't export private Boost implementation types.
# if defined(_WIN32) && defined(LIBLOCKD_SHARED)
#  if defined(BUILDING_LIBLOCKD)
#   define LIBLOCKD_API __declspec(dllexport)
#  else
#   define LIBLOCKD_API __declspec(dllimport)
#  endif
# else
#  define LIBLOCKD_API
# endif

#endif // !defined(LOCKD_LIBLOCKD_API_HPP_INCLUDED_)
