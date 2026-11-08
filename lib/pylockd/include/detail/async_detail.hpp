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
 * File         : async_detail.hpp
 * Part Of      : pylockd - Python integration for liblockd.
 * Purpose      : Provide native asyncio lock operations and Python bindings.
 * Start Date   : 2026-10-21
 *
 * Description  : Python wrappers and futures stay on their owning event-loop thread. The Asio
 *                worker owns protocol sessions and posts completions back to that loop. Tickets
 *                identify operations so cancellation and delayed completion cannot grant a newer
 *                request accidentally. A canceled in-flight acquisition closes its connection,
 *                including a grant already sent by the server but not yet delivered to Python.
 *                Library validation raises pylockd.Exception directly; errors from Python,
 *                asyncio, and context bodies keep their original exception identity.
 *                Scope constructors take mutex references so Boost.Python rejects None before
 *                entering C++. They retain shared ownership for temporary mutex arguments.
 */

#if !defined(LOCKD_PYLOCKD_ASYNC_DETAIL_HPP_INCLUDED_)
# define LOCKD_PYLOCKD_ASYNC_DETAIL_HPP_INCLUDED_

# if defined(_MSC_VER) && (_MSC_VER >= 1020)
#  pragma once
# endif

// Python's feature macros must precede standard headers. Boost's wrapper also
// handles MSVC builds which use a release Python interpreter with a debug CRT.
# include <boost/python.hpp>

# include <atomic>
# include <chrono>
# include <cstdint>
# include <deque>
# include <functional>
# include <map>
# include <memory>
# include <string>
# include <thread>
# include <utility>

# include <boost/asio.hpp>

# include <liblockd/exceptions.hpp>

namespace pld::detail {

namespace py = boost::python;
using boost::asio::ip::tcp;
typedef std::uint64_t ticket_t;

class async_mutex;
class io_session;

class gil_acquire
{
    /* construction and destruction */
public:
    gil_acquire(const gil_acquire&) = delete;
    gil_acquire& operator=(const gil_acquire&) = delete;
    gil_acquire();
    virtual ~gil_acquire();

    /* private data members */
private:
    const PyGILState_STATE _state;
};

// Destructors and coroutine unwinding can enter Python while another exception is pending.
// Temporarily clear that indicator for cleanup, then restore the original type and traceback.
class python_error_guard
{
    /* construction and destruction */
public:
    python_error_guard(const python_error_guard&) = delete;
    python_error_guard& operator=(const python_error_guard&) = delete;
    python_error_guard();
    virtual ~python_error_guard();

    /* private data members */
private:
    PyObject* _type;
    PyObject* _value;
    PyObject* _traceback;
};

// Only this reference crosses to the Asio thread. Python-owned binding objects stay on the
// event-loop thread; the worker acquires the GIL only to enqueue a completion callback.
class loop_reference
{
    /* construction and destruction */
public:
    loop_reference(const loop_reference&) = delete;
    loop_reference& operator=(const loop_reference&) = delete;
    explicit loop_reference(const py::object& loop);
    virtual ~loop_reference();

    /* public methods */
public:
    [[nodiscard]] PyObject* get() const;

    /* private data members */
private:
    PyObject* const _loop;
};

// A worker result contains no Future or scope reference. call_soon_threadsafe delivers it to
// the event loop, where the weak target and operation ticket are checked before changing state.
struct completion
{
    /* public methods */
public:
    void call() const;

    /* public data members */
public:
    const std::weak_ptr<async_mutex> target;
    const ticket_t ticket;
    const bool success;
    const lld::exception::error_type_t error_type;
    const std::string message;
    const bool acquired;
};

// One lazily created reactor services all async sockets. The session registry is worker-owned
// and weak: it permits orderly shutdown without keeping abandoned mutexes alive. Shutdown closes
// sessions and drains canceled handlers before joining the worker; it runs before Python teardown.
class io_runtime
{
    /* construction and destruction */
public:
    io_runtime(const io_runtime&) = delete;
    io_runtime& operator=(const io_runtime&) = delete;
    io_runtime();
    virtual ~io_runtime();

    /* public methods */
public:
    boost::asio::io_context& service();
    [[nodiscard]] bool stopped() const;
    void remember(io_session* const key, const std::shared_ptr<io_session>& session);
    void forget(io_session* const key);
    void stop();

    /* private data members */
private:
    boost::asio::io_context _service;
    boost::asio::executor_work_guard<boost::asio::io_context::executor_type> _work;
    std::map<io_session*, std::weak_ptr<io_session>> _sessions;
    std::atomic<bool> _stopped{false};
    std::thread _thread;
};

// liblockd's synchronous operations wait for replies internally. This binding needs to own
// its socket so cancellation can interrupt a request without changing that client API.
// Network state belongs exclusively to the Asio thread. One reactor services every waiting
// connection; waiting for a lock does not occupy a thread of its own. Disconnecting also
// releases any daemon-side ownership, including a grant racing with cancellation.
class io_session : public std::enable_shared_from_this<io_session>
{
    /* construction and destruction */
public:
    io_session(const io_session&) = delete;
    io_session& operator=(const io_session&) = delete;
    io_session(
            std::shared_ptr<io_runtime> runtime,
            const py::object& loop,
            std::weak_ptr<async_mutex> target,
            const ticket_t ticket,
            std::string hostname,
            const std::uint16_t port,
            std::string name,
            const std::chrono::seconds& connect_timeout
    );
    virtual ~io_session();

    /* public methods, event-loop entry points */
public:
    // These methods post work to the reactor; they don't mutate socket state on the Python thread.
    void start();
    void
    request_lock(const ticket_t ticket, const bool try_only, const std::chrono::milliseconds& wait);
    void release(const ticket_t ticket);
    void cancel();

    // Unlike the posting entry points, close() runs only on the reactor thread.
    void close();

    /* private methods, protocol */
private:
    void _connect();
    void _connectNext();
    void _requestLock(const bool try_only, const std::chrono::milliseconds& wait);
    void _write(const std::string& message, const std::function<void()>& next);
    void
    _read(const std::string& expected,
          const std::function<void()>& next,
          const std::chrono::seconds& timeout = std::chrono::seconds(30));
    void _readReply(
            const std::function<void(const std::string&)>& next,
            const std::chrono::seconds& timeout = std::chrono::seconds(30)
    );
    void _armTimer(const double seconds, const lld::exception::error_type_t error_type);
    void _cancelTimer();
    void _fail(const lld::exception::error_type_t error_type, const std::string& message);
    void
    _notify(const bool success,
            const lld::exception::error_type_t error_type,
            const std::string& message,
            const bool acquired = true);

    /* private data members */
private:
    // Keep the service alive until its sockets and timers have been destroyed, even when
    // Python releases a scope after the atexit hook has stopped the worker.
    const std::shared_ptr<io_runtime> _runtime;
    const std::shared_ptr<loop_reference> _loop;
    const std::weak_ptr<async_mutex> _target;


    // Every command gets its caller's ticket. A delayed completion must never satisfy a newer
    // command, even when the same persistent connection is reused for another scope.
    ticket_t _ticket;
    const std::string _hostname;
    const std::string _port;
    const std::string _name;
    const std::chrono::seconds _connectTimeout;
    tcp::resolver::results_type _endpoints;
    tcp::resolver::results_type::const_iterator _endpoint;
    boost::system::error_code _connectError;
    bool _connectTimedOut = false;
    tcp::resolver _resolver;
    tcp::socket _socket;
    boost::asio::steady_timer _timer;
    boost::asio::streambuf _input;
    std::string _output;


    // Asio cancellation can leave a ready timer handler queued. Its generation must still match
    // before it can close a socket now being used by the next protocol step.
    ticket_t _timerGeneration = 0;
    bool _closed = false;
};

typedef enum
{
    op_connect,
    op_lock,
    op_enter,
    op_try_lock,
    op_timed_lock,
    op_unlock,
    op_exit
} operation_type_t;

// The event loop owns pending requests. Scope and task objects are Python weak references so
// dropping an unawaited operation can run scope cleanup instead of leaving a C++ reference cycle.
// primary_error preserves a context-body exception if releasing its lock also fails.
struct request
{
    /* public data members */
public:
    // deque::erase moves subsequent requests into the erased slot, so these stay assignable.
    ticket_t ticket;
    ticket_t owner;
    operation_type_t type;
    py::object future;
    py::object scope;
    py::object primary_error;
    double wait_milliseconds;
    py::object task;
    py::object timer;
};

// Nested scopes from one task share a lifetime token for one remote acquisition. The last claim
// releases the remote lock, including when scopes are destroyed or released out of nesting order.
struct recursive_ownership
{
    /* construction and destruction */
public:
    virtual ~recursive_ownership() = default;
};

struct recursive_claim
{
    /* public data members */
public:
    const ticket_t ticket;
    const std::shared_ptr<recursive_ownership> ownership;
};

struct cancel_callback
{
    /* public methods */
public:
    void call(const py::object& future) const;

    /* public data members */
public:
    const std::weak_ptr<async_mutex> target;
    const ticket_t ticket;
};

struct queue_timeout_callback
{
    /* public methods */
public:
    void call() const;

    /* public data members */
public:
    const std::weak_ptr<async_mutex> target;
    const ticket_t ticket;
};

// All frontend state is used with the GIL on its owning Python event loop. Only the io_session
// and immutable completion records cross to the worker; futures are never mutated there.
// The native C++ mutex defines validation and wire behavior, but its blocking operations cannot
// be interrupted through asyncio. This frontend uses the shared client_contract helpers and its
// own io_session so cancellation can close an uncertain remote acquisition promptly.
class async_mutex : public std::enable_shared_from_this<async_mutex>
{
    /* construction and destruction */
public:
    async_mutex(const async_mutex&) = delete;
    async_mutex& operator=(const async_mutex&) = delete;
    async_mutex(
            std::string hostname,
            const std::uint16_t port,
            const std::int64_t connect_timeout,
            const std::string& name,
            const bool recursive = false
    );
    virtual ~async_mutex();

    /* public methods */
public:
    ticket_t new_owner();
    bool owns(const ticket_t owner) const;
    std::pair<py::object, ticket_t>
    begin(const ticket_t owner,
          const operation_type_t type,
          const py::object& scope,
          const py::object& primary_error,
          const double wait_milliseconds);
    void cancel(const ticket_t ticket);
    void expire_queued(const ticket_t ticket);
    void abandon(const ticket_t owner);
    void complete(const completion& result);

    /* private methods */
private:
    // A normal release clears ownership while retaining the established connection. Discarding
    // also closes it and leaves this mutex disconnected; pending scopes must then fail rather
    // than silently reconnect with ownership that the caller can no longer reason about.
    void _releaseOwnership();
    void _discard();
    void _startNext();

    /* private data members */
private:
    const std::string _hostname;
    const std::uint16_t _port;
    const std::string _name;
    const std::chrono::seconds _connectTimeout;
    const bool _recursive;
    py::object _loop;
    py::object _owningTask;
    std::map<ticket_t, recursive_claim> _claims;


    // A connection has one wire operation in flight. Other scopes queue locally while _owner
    // reserves it, both during acquisition and until the owner's release is acknowledged.
    std::deque<request> _queue;
    std::unique_ptr<request> _active;
    std::shared_ptr<io_session> _session;


    // Owners identify reusable scope objects; tickets identify individual operations on them.
    // Canceling an earlier operation on a scope must not cancel that scope's later acquisition.
    ticket_t _nextTicket = 0;
    ticket_t _nextOwner = 0;
    ticket_t _owner = 0;
    ticket_t _ownershipTicket = 0;
    bool _held = false;
    bool _connectionStarted = false;
    bool _connected = false;
};

// A scope keeps its mutex alive and has a separate owner identity. Its destructor abandons only
// that owner's requests or claim, preserving an outer recursive scope which is still alive.
class async_scoped_lock
{
    /* construction and destruction */
public:
    async_scoped_lock(const async_scoped_lock&) = delete;
    async_scoped_lock& operator=(const async_scoped_lock&) = delete;
    explicit async_scoped_lock(async_mutex& mutex);
    virtual ~async_scoped_lock();

    /* public methods */
public:
    [[nodiscard]] bool owns_lock() const;
    [[nodiscard]] std::pair<py::object, ticket_t>
    begin(const operation_type_t type,
          const py::object& self,
          const py::object& primary_error,
          const double wait_milliseconds) const;
    void cancel(const ticket_t ticket) const;

    /* private data members */
private:
    const std::shared_ptr<async_mutex> _mutex;
    const ticket_t _owner;
};

class async_timed_mutex : public async_mutex
{
    /* construction and destruction */
public:
    async_timed_mutex(
            const std::string& hostname,
            const std::uint16_t port,
            const std::int64_t connect_timeout,
            const std::string& name
    );
    virtual ~async_timed_mutex() override;
};

class async_scoped_timed_lock : public async_scoped_lock
{
    /* construction and destruction */
public:
    explicit async_scoped_timed_lock(async_timed_mutex& mutex);
    virtual ~async_scoped_timed_lock() override;
};

class async_recursive_mutex : public async_mutex
{
    /* construction and destruction */
public:
    async_recursive_mutex(
            const std::string& hostname,
            const std::uint16_t port,
            const std::int64_t connect_timeout,
            const std::string& name
    );
    virtual ~async_recursive_mutex() override;
};

class async_scoped_recursive_lock : public async_scoped_lock
{
    /* construction and destruction */
public:
    explicit async_scoped_recursive_lock(async_recursive_mutex& mutex);
    virtual ~async_scoped_recursive_lock() override;
};

// A native coroutine delegates suspension to a Python Future's iterator. Registering its
// coroutine protocol also makes asyncio.create_task() work, without executing Python source
// or consuming a worker thread for the duration of each acquisition. Creating this object does
// not send a command: the first send/throw starts it on the running loop. Forwarding throw and
// close is essential because task cancellation can race with an already completed grant Future.
class async_operation
{
    /* construction and destruction */
public:
    async_operation(const async_operation&) = delete;
    async_operation& operator=(const async_operation&) = delete;
    async_operation(
            py::object scope,
            const operation_type_t type,
            py::object primary_error,
            const double wait_value = 0,
            const bool absolute_deadline = false
    );
    virtual ~async_operation();

    /* public methods */
public:
    [[nodiscard]] const async_operation& await() const;
    py::object next();
    py::object send(const py::object& value);
    py::object throw_into(const py::tuple& arguments);
    void close();

    /* private methods */
private:
    void _clearReferences();
    [[nodiscard]] const async_scoped_lock& _scopeLock() const;
    void _cancel() const;
    void _start();

    /* private data members */
private:
    py::object _scope;
    const operation_type_t _type;
    const double _waitValue;
    const bool _absoluteDeadline;
    py::object _primaryError;
    py::object _future;
    py::object _iterator;
    ticket_t _ticket = 0;
    bool _started = false;
    bool _finished = false;
};

} // namespace pld::detail

#endif // !defined(LOCKD_PYLOCKD_ASYNC_DETAIL_HPP_INCLUDED_)
