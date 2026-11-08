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
 * File         : sync_detail.cpp
 * Author       : Brett Gmoser
 * Part Of      : pylockd - Python integration for liblockd.
 * Purpose      : Register synchronous Python lock types and exceptions.
 * Start Date   : 2013-03-17
 *
 * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */


#define BOOST_BIND_GLOBAL_PLACEHOLDERS 1

// Python's feature macros must precede standard headers; use Boost's Python ABI wrapper.
#include <boost/python.hpp> // IWYU pragma: keep

#if defined(_MSC_VER) && (_MSC_VER >= 1020)
# pragma comment(lib, "liblockd.lib")
#endif

#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <ratio>
#include <string>
#include <utility>

#include <boost/python/args.hpp>
#include <boost/python/bases.hpp>
#include <boost/python/borrowed.hpp>
#include <boost/python/class.hpp>
#include <boost/python/copy_const_reference.hpp>
#include <boost/python/def.hpp>
#include <boost/python/enum.hpp>
#include <boost/python/errors.hpp>
#include <boost/python/exception_translator.hpp>
#include <boost/python/extract.hpp>
#include <boost/python/handle.hpp>
#include <boost/python/init.hpp>
#include <boost/python/return_arg.hpp>
#include <boost/python/return_value_policy.hpp>
#include <boost/python/scope.hpp>

#include <liblockd/detail/client_contract.hpp>
#include <liblockd/detail/source_path.hpp>
#include <liblockd/exceptions.hpp>
#include <liblockd/mutex.hpp>
#include <liblockd/recursive_mutex.hpp>
#include <liblockd/timed_mutex.hpp>

#include "../../include/detail/sync_detail.hpp"
#include "../../include/sync.hpp"

using namespace boost::python;

// Restore the GIL before Boost.Python sees a return value or translates a C++ exception. Releasing
// it manually around a call would leave Python without the GIL if that call throws.
gil_release::gil_release()
        : _threadState(PyEval_SaveThread())
{
}


gil_release::~gil_release()
{
    PyEval_RestoreThread(_threadState);
}


static boost::python::object make_lockd_error(const lld::exception& error);


template <typename M>
static std::shared_ptr<lld::mutex> create_mutex(
        const std::string& hostname,
        const std::uint16_t port,
        const std::int64_t connect_timeout,
        const std::string& name
)
{
    // C++ construction connects immediately. Releasing the GIL here lets other Python threads
    // run during DNS, connection setup, and the greeting, just as during later lock operations.
    const gil_release release;
    return std::make_shared<M>(hostname, port, std::chrono::seconds(connect_timeout), name);
}


sync_mutex::sync_mutex(std::shared_ptr<lld::mutex> mutex)
        : _mutexPtr(std::move(mutex))
        , _operationMutex(std::make_shared<std::mutex>())
{
}


sync_mutex::sync_mutex(
        const std::string& hostname,
        const std::uint16_t port,
        const std::int64_t connect_timeout,
        const std::string& name
)
        : sync_mutex(create_mutex<lld::mutex>(hostname, port, connect_timeout, name))
{
}


sync_mutex::~sync_mutex()
{
    // Destruction is reverse declaration order: unlock _operationMutex before restoring the
    // GIL. Otherwise another wrapper could hold this mutex while waiting to reacquire the GIL.
    const gil_release release;
    const std::lock_guard<std::mutex> lock(*_operationMutex);
    _mutexPtr.reset();
}


sync_timed_mutex::sync_timed_mutex(
        const std::string& hostname,
        const std::uint16_t port,
        const std::int64_t connect_timeout,
        const std::string& name
)
        : sync_mutex(create_mutex<lld::timed_mutex>(hostname, port, connect_timeout, name))
{
}


sync_timed_mutex::~sync_timed_mutex() = default;


sync_recursive_mutex::sync_recursive_mutex(
        const std::string& hostname,
        const std::uint16_t port,
        const std::int64_t connect_timeout,
        const std::string& name
)
        : sync_mutex(create_mutex<lld::recursive_mutex>(hostname, port, connect_timeout, name))
{
}


sync_recursive_mutex::~sync_recursive_mutex() = default;


sync_scoped_lock::sync_scoped_lock(const sync_mutex& mutex)
        : _mutexPtr(mutex._mutexPtr)
        , _operationMutex(
                  dynamic_cast<lld::recursive_mutex*>(_mutexPtr.get())
                          ? std::make_shared<std::mutex>()
                          : mutex._operationMutex
          )
        , _lockPtr(std::make_shared<std::unique_lock<lld::mutex>>(*_mutexPtr, std::defer_lock))
{
}


sync_scoped_lock::~sync_scoped_lock()
{
    const gil_release release;
    const std::lock_guard<std::mutex> lock(*_operationMutex);

    // Destroy the guard before its native client, with the GIL released for network cleanup.
    _lockPtr.reset();
    _mutexPtr.reset();
}


const sync_scoped_lock& sync_scoped_lock::enter() const
{
    lock();
    return *this;
}


void sync_scoped_lock::exit(const object&, const object& error, const object&) const
{
    try {
        unlock();
    }
    catch (const lld::exception& cleanup) {
        if (error.is_none())
            throw;

        // Returning normally keeps __exit__ from replacing the body exception. SetCause steals
        // a reference, so give it its own reference while retaining the temporary Python object.
        const object cause = make_lockd_error(cleanup);
        Py_INCREF(cause.ptr());
        PyException_SetCause(error.ptr(), cause.ptr());
    }
}


bool sync_scoped_lock::owns_lock() const
{
    const gil_release release;
    const std::lock_guard<std::mutex> lock(*_operationMutex);
    return _lockPtr->owns_lock();
}


void sync_scoped_lock::_checkOwnership(const bool expected) const
{
    if (_lockPtr->owns_lock() != expected)
        throw lld::exception(
                lld::exception::et_logic_error,
                LOCKD_SOURCE_FILE,
                __LINE__,
                __FUNCTION__,
                expected ? "This ScopedLock does not own the lock"
                         : "This ScopedLock already owns the lock"
        );
}


void sync_scoped_lock::lock() const
{
    const gil_release release;
    const std::lock_guard<std::mutex> lock(*_operationMutex);
    _checkOwnership(false);
    _lockPtr->lock();
}


void sync_scoped_lock::unlock() const
{
    const gil_release release;
    const std::lock_guard<std::mutex> lock(*_operationMutex);
    _checkOwnership(true);

    // Explicit Python operations use C++'s throwing unlock mode. A wrong-thread recursive release
    // leaves native ownership unchanged, so keep the guard owned on that local usage error.
    // A failed remote release instead discards the connection and clears the guard.
    try {
        _mutexPtr->unlock(lld::mutex::tt_throw_on_failure);
    }
    catch (const lld::exception& error) {
        if (error.error_type() == lld::exception::et_logic_error)
            throw;
        _lockPtr->release();
        *_lockPtr = std::unique_lock<lld::mutex>(*_mutexPtr, std::defer_lock);
        throw;
    }
    _lockPtr->release();
    *_lockPtr = std::unique_lock<lld::mutex>(*_mutexPtr, std::defer_lock);
}


bool sync_scoped_lock::try_lock() const
{
    const gil_release release;
    const std::lock_guard<std::mutex> lock(*_operationMutex);
    _checkOwnership(false);
    return _lockPtr->try_lock();
}


sync_scoped_timed_lock::sync_scoped_timed_lock(const sync_timed_mutex& mutex)
        : sync_scoped_lock(mutex)
{
}


sync_scoped_timed_lock::~sync_scoped_timed_lock() = default;


bool sync_scoped_timed_lock::try_lock_until(const double abs_time) const
{
    const double now =
            std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch())
                    .count();
    return try_lock_for(
            std::chrono::duration<double, std::milli>(
                    lld::detail::lock_wait_milliseconds((abs_time - now) * 1000.0)
            )
                    .count()
    );
}


bool sync_scoped_timed_lock::try_lock_for(const double ms) const
{
    const auto wait = lld::detail::lock_wait_milliseconds(ms);
    const gil_release release;
    const std::lock_guard<std::mutex> lock(*_operationMutex);
    _checkOwnership(false);
    auto* const mutex = dynamic_cast<lld::timed_mutex*>(_mutexPtr.get());
    if (!mutex->try_lock_for(wait))
        return false;

    // The daemon already granted ownership; adopt it without invoking another native lock().
    *_lockPtr = std::unique_lock<lld::mutex>(*_mutexPtr, std::adopt_lock);
    return true;
}


sync_scoped_recursive_lock::sync_scoped_recursive_lock(const sync_recursive_mutex& mutex)
        : sync_scoped_lock(mutex)
{
}


sync_scoped_recursive_lock::~sync_scoped_recursive_lock() = default;


static PyObject* lockd_exception_type = nullptr;

static boost::python::object make_lockd_error(const lld::exception& error)
{
    object instance(handle<>(PyObject_CallFunction(lockd_exception_type, "s", error.what())));
    instance.attr("cause") = object(error);
    instance.attr("what") = object(error.what());
    return instance;
}


static void translate_lockd_exception(const lld::exception& error)
{
    const object instance = make_lockd_error(error);
    PyErr_SetObject(lockd_exception_type, instance.ptr());
}


static PyObject*
createExceptionClass(const char* const name, PyObject* const baseTypeObj = PyExc_Exception)
{
    // https://stackoverflow.com/a/65129096/1751757
    using std::string;
    namespace bp = boost::python;

    // Keep the Python name alive while copying its UTF-8 bytes. Extracting a C string
    // avoids GCC's uninitialized-storage diagnostic for Boost's temporary std::string converter.
    const object scopeName = bp::scope().attr("__name__");
    const char* const scope_name_bytes = bp::extract<const char*>(scopeName);
    const string qualifiedName0 = string(scope_name_bytes) + "." + name;
    PyObject* const typeObj = PyErr_NewException(qualifiedName0.c_str(), baseTypeObj, nullptr);
    if (!typeObj)
        bp::throw_error_already_set();
    bp::scope().attr(name) = bp::handle<>(bp::borrowed(typeObj));
    return typeObj;
}

static void raiser()
{
    throw lld::exception(
            lld::exception::et_not_connected,
            LOCKD_SOURCE_FILE,
            __LINE__,
            __FUNCTION__,
            "Not connected to lockd."
    );
}

void pld::export_sync_types()
{
    // @formatter:off
    using namespace boost::python;

    // Wrap the native error metadata stored in each translated exception's .cause. Below,
    // createExceptionClass replaces the public name with a subclass of Python's Exception;
    // Boost.Python keeps this wrapper registered for conversion of the native metadata.
    class_<lld::exception>(
            "Exception",
            init<const lld::exception::error_type_t&,
                 const std::string&,
                 const unsigned int,
                 const std::string&,
                 const std::string&>()
    )
            .def(
                    init<const lld::exception::error_type_t&,
                         const std::string&,
                         const unsigned int,
                         const std::string&>()
            )
            .def(init<const lld::exception::error_type_t&, const std::string&>())
            .def("what", &lld::exception::what)
            .def("error_type",
                 &lld::exception::error_type,
                 return_value_policy<copy_const_reference>())
            .def("file", &lld::exception::file, return_value_policy<copy_const_reference>())
            .def("function", &lld::exception::function, return_value_policy<copy_const_reference>())
            .def("line", &lld::exception::line, return_value_policy<copy_const_reference>());

    enum_<lld::exception::error_type_t>("ErrorType")
            .value("unimplemented", lld::exception::et_unimplemented)
            .value("log_error", lld::exception::et_logic_error)
            .value("lockd_unavailable", lld::exception::et_lockd_unavailable)
            .value("read_error", lld::exception::et_read_error)
            .value("write_error", lld::exception::et_write_error)
            .value("not_connected", lld::exception::et_not_connected)
            .value("unexpected", lld::exception::et_unexpected)
            .value("unknown_exception", lld::exception::et_unknown_exception)
            .export_values();

    lockd_exception_type = createExceptionClass("Exception");
    register_exception_translator<lld::exception>(&translate_lockd_exception);

    def("raiser", &raiser);

    {
        // Construction registers the Python type; this is not an exception or a guard.
        // NOLINTNEXTLINE(bugprone-throw-keyword-missing, bugprone-unused-raii)
        class_<sync_mutex>(
                "Mutex",
                init<const std::string&,
                     const std::uint16_t,
                     const std::int64_t,
                     const std::string&>(
                        (arg("hostname"), arg("port"), arg("connect_timeout"), arg("lock_name"))
                )
        );
        class_<sync_scoped_lock>("ScopedLock", init<sync_mutex&>())
                .def("__enter__", &sync_scoped_lock::enter, return_self<>())
                .def("__exit__", &sync_scoped_lock::exit)
                .def("owns_lock", &sync_scoped_lock::owns_lock)
                .def("lock", &sync_scoped_lock::lock)
                .def("unlock", &sync_scoped_lock::unlock)
                .def("try_lock", &sync_scoped_lock::try_lock);

        // Construction registers the Python type; this is not an exception or a guard.
        // NOLINTNEXTLINE(bugprone-throw-keyword-missing, bugprone-unused-raii)
        class_<sync_timed_mutex, bases<sync_mutex>>(
                "TimedMutex",
                init<const std::string&,
                     const std::uint16_t,
                     const std::int64_t,
                     const std::string&>(
                        (arg("hostname"), arg("port"), arg("connect_timeout"), arg("lock_name"))
                )
        );

        class_<sync_scoped_timed_lock, bases<sync_scoped_lock>>(
                "ScopedTimedLock", init<sync_timed_mutex&>()
        )
                .def("__enter__", &sync_scoped_lock::enter, return_self<>())
                .def("__exit__", &sync_scoped_lock::exit)
                .def("owns_lock", &sync_scoped_timed_lock::owns_lock)
                .def("lock", &sync_scoped_timed_lock::lock)
                .def("timed_lock", &sync_scoped_timed_lock::lock)
                .def("unlock", &sync_scoped_timed_lock::unlock)
                .def("try_lock", &sync_scoped_timed_lock::try_lock)
                .def("try_lock_until", &sync_scoped_timed_lock::try_lock_until)
                .def("try_lock_for", &sync_scoped_timed_lock::try_lock_for);
    }
    {
        // Construction registers the Python type; this is not an exception or a guard.
        // NOLINTNEXTLINE(bugprone-throw-keyword-missing, bugprone-unused-raii)
        class_<sync_recursive_mutex, bases<sync_mutex>>(
                "RecursiveMutex",
                "A remote mutex which permits balanced acquisitions by the owning thread. "
                "Recursion is per instance; distinct instances with one name still compete.",
                init<const std::string&,
                     const std::uint16_t,
                     const std::int64_t,
                     const std::string&>(
                        (arg("hostname"), arg("port"), arg("connect_timeout"), arg("lock_name"))
                )
        );
        class_<sync_scoped_recursive_lock, bases<sync_scoped_lock>>(
                "ScopedRecursiveLock", init<const sync_recursive_mutex&>()
        )
                .def("__enter__", &sync_scoped_recursive_lock::enter, return_self<>())
                .def("__exit__", &sync_scoped_recursive_lock::exit)
                .def("owns_lock", &sync_scoped_recursive_lock::owns_lock)
                .def("lock", &sync_scoped_recursive_lock::lock)
                .def("unlock", &sync_scoped_recursive_lock::unlock)
                .def("try_lock", &sync_scoped_recursive_lock::try_lock);
    }
    // @formatter:on
}
