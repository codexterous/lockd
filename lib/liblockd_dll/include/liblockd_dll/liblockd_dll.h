/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * 
 * Codexterous - Software developed with you in mind.
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
 * File       : liblockd_dll.h
 * Author     : Brett Gmoser
 * Part Of    : liblockd_dll, a C client library DLL for interfacing with lockd.
 * Purpose    : C ABI for mutex handles, lock guards, errors, and version strings.
 * Start Date : 09/27/2012
 *
 * Description: This standalone C99 header also provides C linkage to C++ callers. Mutex handles
 *              own lld::mutex connections; lock handles own deferred std::unique_lock guards.
 *              Creating a guard does not acquire ownership. Destroy guards before their mutexes;
 *              destroying an owning guard releases its lock through the client's cleanup path.
 *
 *              Creation and lock operations catch standard C++ exceptions, store their messages
 *              in thread-local storage, and return the documented failure result. Error copying
 *              preserves NUL termination for every valid nonempty output buffer. The caller must
 *              provide valid handles and serialize access to each connection.
 *
 *              LLDAPI selects Windows import/export annotations for shared builds and is empty
 *              for static builds and Unix libraries. C++-only version helpers stay in version.hpp.
 *
 * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

#if !defined(LOCKD_LIBLOCKD_DLL_LIBLOCKD_DLL_H_INCLUDED_)
# define LOCKD_LIBLOCKD_DLL_LIBLOCKD_DLL_H_INCLUDED_

# if defined(_MSC_VER) && (_MSC_VER >= 1020)
#  pragma once
# endif

# if (defined(WIN32) || defined(_WIN32)) && !defined(LIBLOCKD_DLL_STATIC)
#  ifdef BUILDING_DLL
#   define LLDAPI __declspec(dllexport)
#  else
#   define LLDAPI __declspec(dllimport)
#  endif // BUILDING_DLL
# else
#  define LLDAPI /**/
# endif          // WIN32

# ifndef __cplusplus
#  include <stdbool.h>
# endif
# include <stddef.h>
# include <stdint.h>

# ifdef __cplusplus
extern "C" {
# endif

// Function   : create_mutex(server, port, timeout, lock_name)
// Description: Creates a handle to a lockd mutex, which connects to lockd.
// Parameters : server - The address to the server (probably localhost).
//              port - the port to connect to (probably 7198).
//              timeout - Nonnegative connection timeout seconds within the steady clock range.
//              lock_name - The name of the lock.
// Returns    : A handle to the newly created lockd mutex.
// Notes      : The handle must be destroyed (with the destroy_mutex function) as soon as you are
//              done using it.
//
//              Returns NULL on failure (such as inability to connect to the lockd server).
//
//              Locks MUST be destroyed before the mutexes they belong to.
//
LLDAPI void* create_mutex(const char* const, const uint16_t, const int64_t, const char* const);

// Function   : destroy_mutex(mptr)
// Description: Destroys a lockd mutex.
// Parameters : mptr - Handle to a lockd mutex (created with create_mutex).
// Returns    : Nothing.
//
LLDAPI void destroy_mutex(void* const);

// Function   : create_lock(mptr)
// Description: Creates a handle to a lockd lock, which can later be locked.
// Parameters : mptr - Handle to a lockd mutex (created with create_mutex).
// Returns    : A handle to the newly created lock.
// Notes      : The handle must be destroyed (with the destroy_lock function) as soon as you are
//              done using it.
//
//              Returns NULL on failure (such as the lockd server no longer being available).
//
LLDAPI void* create_lock(void* const);

// Function   : destroy_lock(lptr)
// Description: Destroys a lockd lock.
// Parameters : lptr - Handle to a lockd lock (created with create_lock).
// Returns    : Nothing.
// Notes      : Locks MUST be destroyed BEFORE the mutexes they belong to.
//
LLDAPI void destroy_lock(void* const);

// Function   : lock(lptr)
// Description: Blocks while obtaining a lock on the mutex.
// Parameters : lptr - Handle to a lockd lock (created with create_lock).
// Returns    : true if successful, false if a catastrophic error occurred and an exception is
//              caught. This may mean the connection to lockd was severed.
// Notes      : This function will block indefinitely while waiting to obtain exclusive access on
//              the mutex.
//
LLDAPI bool lock(void* const);

// Function   : try_lock(lptr)
// Description: Try to immediately get exclusive access on the mutex, returning immediately if the
//              function would otherwise block.
// Parameters : lptr - Handle to a lockd lock (created with create_lock).
// Returns    : 0 - The lock was able to be immediately acquired.
//              1 - The lock was not able to be immediately acquired, and no lock has been obtained.
//              2 - A catastrophic error occurred and an exception is caught. This may mean the
//                  connection to lockd was severed.
//
LLDAPI int32_t try_lock(void* const);

// Function   : unlock(lptr)
// Description: Unlock the lock, allowing other clients access to the named mutex.
// Parameters : lptr - Handle to a lockd lock (created with create_lock).
// Returns    : true if successful, false if a catastrophic error occurred and an exception is
//              caught. This may mean the connection to lockd was severed.
// Notes      : Unlocking is done automatically upon destroying a lock, so if you do not need to
//              explicitly unlock a lock for any reason, this is unnecessary.
//
LLDAPI bool unlock(void* const);

// Function   : get_last_error(buf, buflen)
// Description: If an error occurs, the error is stored in a thread-safe way to thread-local
//              storage. Call get_last_error to obtain information about the error that occurred.
// Parameters : buf - A buffer in which to store the string.
//              buflen - The length of the buffer.
// Returns    : true if successful, false if successful but the output buffer did not contain
//              enough space to store the entire error message according to the buflen parameter,
//              or if no error message exists for the current thread. Capacity includes the NUL;
//              at most buflen - 1 bytes are copied and the result is always terminated when buf
//              is non-NULL and buflen is nonzero. No error produces an empty string. A truncated
//              nonempty error is distinguishable when buflen is at least 2. 512 bytes should be
//              plenty sufficient. NULL or zero-capacity buffers return false without writing.
//
LLDAPI bool get_last_error(char* const, const size_t);

// Function   : lockd_version(buf, buflen)
// Description: Copies a version information string for liblockd and liblockd_dll.
// Parameters : buf - A buffer in which to store the string.
//              buflen - The length of the buffer.
// Returns    : true if successful, false if successful but the output buffer did not contain
//              enough space to store the entire version string according to the buflen parameter.
//              Capacity includes the NUL; truncated results are terminated. NULL or zero-capacity
//              buffers return false without writing.
// Notes      : Preach it loud and proud.
//
LLDAPI bool liblockd_version(char* const, const size_t);

# ifdef __cplusplus
} // extern "C"
# endif

#endif // !defined(LOCKD_LIBLOCKD_DLL_LIBLOCKD_DLL_H_INCLUDED_)
