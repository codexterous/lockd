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
 * File       : lock_name.hpp
 * Author     : Brett Gmoser
 * Part Of    : liblockd, a client library for interfacing with lockd.
 * Purpose    : Shared ASCII lock-name grammar for the daemon and client APIs.
 * Start Date : 10/28/2026
 *
 * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

#if !defined(LOCKD_LIBLOCKD_DETAIL_LOCK_NAME_HPP_INCLUDED_)
# define LOCKD_LIBLOCKD_DETAIL_LOCK_NAME_HPP_INCLUDED_

# if defined(_MSC_VER) && (_MSC_VER >= 1020)
#  pragma once
# endif

# include <boost/spirit/home/x3/char/char.hpp>
# include <boost/spirit/home/x3/char/char_class.hpp>
# include <boost/spirit/home/x3/directive/repeat.hpp>
# include <boost/spirit/home/x3/operator/alternative.hpp>

namespace lld::detail {

// ASCII classification is deliberate: locale and UTF-8 bytes must not expand the alphabet.
inline constexpr auto lock_name_characters = boost::spirit::x3::ascii::alnum |
                                             boost::spirit::x3::char_('_') |
                                             boost::spirit::x3::char_('-');
inline constexpr unsigned int max_lock_name_length = 256;
inline constexpr auto lock_name =
        boost::spirit::x3::repeat(1U, max_lock_name_length)[lock_name_characters];

} // namespace lld::detail

#endif // !defined(LOCKD_LIBLOCKD_DETAIL_LOCK_NAME_HPP_INCLUDED_)
