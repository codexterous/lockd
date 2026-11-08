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
 * File       : version.cpp
 * Author     : Brett Gmoser
 * Part Of    : liblockd_dll, a C client library DLL for interfacing with lockd.
 * Purpose    : Provide C interface library version information.
 * Start Date : 09/27/2012
 *
 * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

#include <sstream>
#include <string>

#include "lockd_build_version.hpp"

#include "../include/liblockd_dll/version.hpp"

// Define this function here, separately, so that no other object has to be recompiled
// at compile-time.
void ldd::version(int& major, int& minor, int& build)
{
    major = MAJOR_VERSION;
    minor = MINOR_VERSION;
    build = BUILD_NUMBER;
}

std::string ldd::version_string()
{
    std::stringstream ss;
    ss << "v" << LOCKD_VERSION_STRING;

#ifdef _MSC_VER
# if defined(_M_ARM64)
    ss << " arm64";
# elif defined(_M_X64)
    ss << " x86_64";
# else
    ss << " x86";
# endif
# ifdef _DEBUG
    ss << "/dbg";
# else
    ss << "/opt";
# endif // _DEBUG
#endif  // _MSC_VER

    return ss.str();
}
