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
 * File       : service_fixture.hpp
 * Author     : Brett Gmoser
 * Part Of    : lockd, an interprocess locking daemon
 * Purpose    : Windows elevation boundary doubles for service tests.
 * Start Date : 10/23/2026
 *
 * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

#if !defined(LOCKD_TEST_SERVICE_FIXTURE_HPP_INCLUDED_)
# define LOCKD_TEST_SERVICE_FIXTURE_HPP_INCLUDED_

# if defined(_MSC_VER) && (_MSC_VER >= 1020)
#  pragma once
# endif

# ifdef WIN32

#  include "../../../include/platform/win32/service_elevation.hpp"

namespace ldt {

// Replace only the token query and process launch; run() retains the production decision logic.
class service_elevation_fixture : public lkd::platform::win32::service_elevation
{
    /* construction and destruction */
public:
    virtual ~service_elevation_fixture() override = default;

    /* protected methods */
protected:
    bool elevated() const override;
    int relaunch() const override;

    /* public data members */
public:
    bool token_elevated = false;
    int token_error = 0;
    int launch_error = 0;
    int child_result = 0;
    mutable unsigned int launches = 0;
};

} // namespace ldt

# endif // WIN32

#endif // !defined(LOCKD_TEST_SERVICE_FIXTURE_HPP_INCLUDED_)
