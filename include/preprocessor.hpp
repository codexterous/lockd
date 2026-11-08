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
 * File       : preprocessor.hpp
 * Author     : Brett Gmoser
 * Part Of    : lockd, an interprocess locking daemon
 * Purpose    : Platform attributes and compiler diagnostics shared by the daemon.
 * Start Date : 03/26/2012
 *
 * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

#if !defined(LOCKD_LOCKD_PREPROCESSOR_HPP_INCLUDED_)
# define LOCKD_LOCKD_PREPROCESSOR_HPP_INCLUDED_

# if defined(_MSC_VER) && (_MSC_VER >= 1020)
#  pragma once
#  pragma warning(default : 4265) // enable warning about virtual dtors
# endif

# include <cassert>

// Our NODEFAULT() macro utilizes MSVC's __assume keyword to optimize out any 'default' portions of a switch
// for which there is no default value. Example:
//   enum my_type { something, something_else };
//   std::string to_string(const my_type& e) {
//     switch(e) {
//     case something:
//       return "something";
//     case something_else:
//       return "something_else";
//     default: // can never happen!
//       NODEFAULT();
//     }
//  }
# if defined(_MSC_VER) && (_MSC_VER >= 1020)
#  if defined(DEBUG) || defined(_DEBUG)
#   define NODEFAULT() (assert(0))
#  else
#   define NODEFAULT() (__assume(0))
#  endif
# else
#  define NODEFAULT() assert(0)
# endif

// An UNREACHABLE_RETURN macro helps us in instances where MSVC warns about "not all return paths
// return a value" in debug compiles, and "unreachable code" in release compiles.
# if defined(_MSC_VER) && (defined(_DEBUG) || defined(DEBUG))
#  define UNREACHABLE_RETURN(x) return x;
# elif defined(_MSC_VER)
#  define UNREACHABLE_RETURN(x)
# else
#  define UNREACHABLE_RETURN(x) return x;
# endif // defined(_MSC_VER) && (defined(_DEBUG) || defined(DEBUG))

#endif // !defined(LOCKD_LOCKD_PREPROCESSOR_HPP_INCLUDED_)
