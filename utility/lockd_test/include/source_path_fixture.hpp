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
 * File       : source_path_fixture.hpp
 * Author     : Brett Gmoser
 * Part Of    : lockd, an interprocess locking daemon
 * Purpose    : Exercise source paths across native compilation boundaries.
 * Start Date : 11/11/2026
 *
 * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

#if !defined(LOCKD_TEST_SOURCE_PATH_FIXTURE_HPP_INCLUDED_)
# define LOCKD_TEST_SOURCE_PATH_FIXTURE_HPP_INCLUDED_

# if defined(_MSC_VER) && (_MSC_VER >= 1020)
#  pragma once
# endif

# include <liblockd/detail/source_path.hpp>
# include <liblockd/exceptions.hpp>

namespace ldt {

const char* source_path_from_other_translation_unit();
const char* header_path_from_other_translation_unit();
void throw_from_other_translation_unit();

inline const char* source_path_from_header()
{
    return LOCKD_SOURCE_FILE;
}

// Separate template instantiations exercise the same throwing header from two translation units.
template <typename T> void throw_from_source_path_header()
{
    throw lld::exception(
            lld::exception::et_logic_error, LOCKD_SOURCE_FILE, __LINE__, __FUNCTION__, "header"
    );
}

} // namespace ldt

#endif // !defined(LOCKD_TEST_SOURCE_PATH_FIXTURE_HPP_INCLUDED_)
