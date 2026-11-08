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
 * File         : async_detail.cpp
 * Part Of      : pylockd - Python integration for liblockd.
 * Purpose      : Provide native asyncio lock operations and Python bindings.
 * Start Date   : 2026-10-21
 *
 */

#define BOOST_BIND_GLOBAL_PLACEHOLDERS 1

// Python's feature macros must precede standard headers; use Boost's Python ABI wrapper.
#include <boost/python.hpp> // IWYU pragma: keep

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <istream>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <boost/asio/buffer.hpp>
#include <boost/asio/connect.hpp> // IWYU pragma: keep
#include <boost/asio/post.hpp>

#include <boost/asio/read_until.hpp> // IWYU pragma: keep

#include <boost/asio/write.hpp> // IWYU pragma: keep
#include <boost/core/noncopyable.hpp>
#include <boost/python/args.hpp>
#include <boost/python/bases.hpp>
#include <boost/python/borrowed.hpp>
#include <boost/python/class.hpp>
#include <boost/python/def.hpp>
#include <boost/python/dict.hpp>
#include <boost/python/errors.hpp>
#include <boost/python/extract.hpp>
#include <boost/python/handle.hpp>
#include <boost/python/import.hpp>
#include <boost/python/init.hpp>
#include <boost/python/object.hpp>
#include <boost/python/raw_function.hpp>
#include <boost/python/return_arg.hpp>
#include <boost/python/tuple.hpp>

#include <boost/system/error_code.hpp> // IWYU pragma: keep

#include <liblockd/detail/client_contract.hpp>
#include <liblockd/detail/source_path.hpp>
#include <liblockd/exceptions.hpp>

#include "../../include/async.hpp"
#include "../../include/detail/async_detail.hpp"

namespace pld::detail {

namespace {

py::object make_error(const lld::exception::error_type_t type, const std::string& message)
{
    py::object instance = py::import("pylockd").attr("Exception")(message);
    instance.attr("what") = message;
    instance.attr("cause") =
            lld::exception(type, LOCKD_SOURCE_FILE, __LINE__, __FUNCTION__, message);
    return instance;
}


void logic_error(const char* const message)
{
    const py::object error = make_error(lld::exception::et_logic_error, message);
    PyErr_SetObject(reinterpret_cast<PyObject*>(Py_TYPE(error.ptr())), error.ptr());
    py::throw_error_already_set();
}


constexpr bool is_acquisition(const operation_type_t type)
{
    return type == op_lock || type == op_enter || type == op_try_lock || type == op_timed_lock;
}


double system_seconds()
{
    return std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch())
            .count();
}


} // namespace

gil_acquire::gil_acquire()
        : _state(PyGILState_Ensure())
{
}


gil_acquire::~gil_acquire()
{
    PyGILState_Release(_state);
}

python_error_guard::python_error_guard()
        : _type(nullptr)
        , _value(nullptr)
        , _traceback(nullptr)
{
    PyErr_Fetch(&_type, &_value, &_traceback);
}


python_error_guard::~python_error_guard()
{
    PyErr_Restore(_type, _value, _traceback);
}

loop_reference::loop_reference(const py::object& loop)
        : _loop(loop.ptr())
{
    Py_INCREF(_loop);
}


loop_reference::~loop_reference()
{
    if (Py_IsInitialized()) {
        const gil_acquire lock;
        Py_DECREF(_loop);
    }
}


PyObject* loop_reference::get() const
{
    return _loop;
}


io_runtime::io_runtime()
        : _work(_service.get_executor())
        , _thread([this] { _service.run(); })
{
}


io_runtime::~io_runtime()
{
    stop();
}


boost::asio::io_context& io_runtime::service()
{
    return _service;
}


bool io_runtime::stopped() const
{
    return _stopped;
}


void io_runtime::remember(io_session* const key, const std::shared_ptr<io_session>& session)
{
    _sessions[key] = session;
}


void io_runtime::forget(io_session* const key)
{
    _sessions.erase(key);
}

namespace {

std::shared_ptr<io_runtime> IO_RUNTIME;
bool ASYNC_SHUTDOWN = false;

std::shared_ptr<io_runtime> get_runtime()
{
    if (ASYNC_SHUTDOWN)
        logic_error("pylockd's async runtime has shut down");
    if (!IO_RUNTIME)
        IO_RUNTIME = std::make_shared<io_runtime>();
    return IO_RUNTIME;
}

} // namespace

io_session::io_session(
        std::shared_ptr<io_runtime> runtime,
        const py::object& loop,
        std::weak_ptr<async_mutex> target,
        const ticket_t ticket,
        std::string hostname,
        const std::uint16_t port,
        std::string name,
        const std::chrono::seconds& connect_timeout
)
        : _runtime(std::move(runtime))
        , _loop(std::make_shared<loop_reference>(loop))
        , _target(std::move(target))
        , _ticket(ticket)
        , _hostname(std::move(hostname))
        , _port(std::to_string(port))
        , _name(std::move(name))
        , _connectTimeout(connect_timeout)
        , _resolver(_runtime->service())
        , _socket(_runtime->service())
        , _timer(_runtime->service())
        , _input(lld::detail::reply_buffer_limit)
{
}


io_session::~io_session() = default;


void io_session::start()
{
    boost::asio::post(_runtime->service(), [self = shared_from_this()] {
        self->_runtime->remember(self.get(), self);
        self->_connect();
    });
}


void io_session::release(const ticket_t ticket)
{
    boost::asio::post(_runtime->service(), [self = shared_from_this(), ticket] {
        if (self->_closed)
            return;
        self->_ticket = ticket;
        self->_write("UNLOCK\n", [self] {
            self->_read("LOCK_RELEASED", [self] {
                self->_notify(true, lld::exception::et_unexpected, "");
            });
        });
    });
}


void io_session::cancel()
{
    // Shutdown already closes every registered socket before joining the worker. A later
    // Python destructor must not enqueue work on that stopped service.
    if (!_runtime->stopped())
        boost::asio::post(_runtime->service(), [self = shared_from_this()] { self->close(); });
}


void io_session::close()
{
    if (_closed)
        return;
    _closed = true;
    _cancelTimer();
    _resolver.cancel();
    boost::system::error_code ignored;

    // A pending read can make close() reset the connection on Windows. Shut down
    // first so cancellation and timeout cleanup also give the peer an orderly EOF.
    // NOLINTNEXTLINE(bugprone-unused-return-value)
    static_cast<void>(_socket.shutdown(tcp::socket::shutdown_both, ignored));

    // NOLINTNEXTLINE(bugprone-unused-return-value)
    static_cast<void>(_socket.close(ignored));
    _runtime->forget(this);
}


void io_session::_connect()
{
    // Resolution precedes the per-endpoint connection deadline in the native C++ client too.
    _resolver.async_resolve(
            _hostname,
            _port,
            [self = shared_from_this()](
                    const boost::system::error_code& error,
                    const tcp::resolver::results_type& endpoints
            ) {
                if (self->_closed)
                    return;
                if (error) {
                    self->_fail(lld::exception::et_lockd_unavailable, error.message());
                    return;
                }
                self->_endpoints = endpoints;
                self->_endpoint = self->_endpoints.begin();
                self->_connectNext();
            }
    );
}


void io_session::_connectNext()
{
    if (_endpoint == _endpoints.end()) {
        _fail(lld::exception::et_lockd_unavailable, _connectError.message());
        return;
    }
    _connectTimedOut = false;
    const auto generation = ++_timerGeneration;
    _timer.expires_after(_connectTimeout);
    _timer.async_wait([self = shared_from_this(),
                       generation](const boost::system::error_code& error) {
        if (!error && !self->_closed && generation == self->_timerGeneration) {
            self->_connectTimedOut = true;
            boost::system::error_code ignored;

            // NOLINTNEXTLINE(bugprone-unused-return-value)
            static_cast<void>(self->_socket.close(ignored));
        }
    });
    const tcp::endpoint endpoint = (_endpoint++)->endpoint();
    _socket.async_connect(
            endpoint, [self = shared_from_this()](const boost::system::error_code& error) {
                if (self->_closed)
                    return;
                self->_cancelTimer();
                self->_connectError = self->_connectTimedOut
                                              ? boost::system::errc::make_error_code(
                                                        boost::system::errc::timed_out
                                                )
                                              : error;
                if (self->_connectError) {
                    boost::system::error_code ignored;

                    // NOLINTNEXTLINE(bugprone-unused-return-value)
                    static_cast<void>(self->_socket.close(ignored));
                    self->_connectNext();
                    return;
                }
                self->_read(
                        "LOCKD",
                        [self] { self->_notify(true, lld::exception::et_unexpected, ""); },
                        lld::detail::greeting_timeout
                );
            }
    );
}


void io_session::request_lock(
        const ticket_t ticket, const bool try_only, const std::chrono::milliseconds& wait
)
{
    boost::asio::post(_runtime->service(), [self = shared_from_this(), ticket, try_only, wait] {
        if (self->_closed)
            return;
        self->_ticket = ticket;
        self->_requestLock(try_only, wait);
    });
}


void io_session::_requestLock(const bool try_only, const std::chrono::milliseconds& wait)
{
    if (try_only) {
        _write("TRY_LOCK " + _name + "\n", [self = shared_from_this()] {
            self->_readReply([self](const std::string& reply) {
                if (reply == "LOCK_ACQUIRED")
                    self->_notify(true, lld::exception::et_unexpected, "");
                else if (reply == "LOCK_UNAVAILABLE")
                    self->_notify(true, lld::exception::et_unexpected, "", false);
                else
                    self->_fail(
                            lld::exception::et_unexpected,
                            "lockd returned unexpected response (" + reply + ")"
                    );
            });
        });
        return;
    }

    const bool timed = wait > std::chrono::milliseconds::zero();
    const std::string command =
            timed ? "TIMED_LOCK " + _name + " " + std::to_string(wait.count()) + "ms\n"
                  : "LOCK " + _name + "\n";
    _write(command, [self = shared_from_this(), timed] {
        self->_read(
                "LOCK_ACQUIRING",
                [self, timed] {
                    // The daemon owns the wait deadline. A local timer would include setup and could
                    // discard a grant that the C++ client accepts. Cancellation still closes the socket.
                    self->_readReply(
                            [self, timed](const std::string& reply) {
                                if (reply == "LOCK_ACQUIRED")
                                    self->_notify(true, lld::exception::et_unexpected, "");
                                else if (timed && reply == "LOCK_TIMED_OUT")
                                    self->_notify(true, lld::exception::et_unexpected, "", false);
                                else
                                    self->_fail(
                                            lld::exception::et_unexpected,
                                            "lockd returned unexpected response (" + reply + ")"
                                    );
                            },
                            std::chrono::seconds::zero()
                    );
                },
                timed ? lld::detail::timed_ack_timeout : lld::detail::command_timeout
        );
    });
}


void io_session::_write(const std::string& message, const std::function<void()>& next)
{
    // The buffer must outlive async_write, including cancellation and partial writes.
    _output = message;
    boost::asio::async_write(
            _socket,
            boost::asio::buffer(_output),
            [self = shared_from_this(),
             next](const boost::system::error_code& error, const std::size_t) {
                if (self->_closed)
                    return;
                self->_cancelTimer();
                if (error)
                    self->_fail(lld::exception::et_write_error, error.message());
                else
                    next();
            }
    );
}


void io_session::_read(
        const std::string& expected,
        const std::function<void()>& next,
        const std::chrono::seconds& timeout
)
{
    _readReply(
            [self = shared_from_this(), expected, next](const std::string& reply) {
                if (reply != expected)
                    self->_fail(
                            lld::exception::et_unexpected,
                            "lockd returned unexpected response (" + reply + ")"
                    );
                else
                    next();
            },
            timeout
    );
}


void io_session::_readReply(
        const std::function<void(const std::string&)>& next, const std::chrono::seconds& timeout
)
{
    if (timeout != std::chrono::seconds::zero())
        _armTimer(static_cast<double>(timeout.count()), lld::exception::et_read_error);
    boost::asio::async_read_until(
            _socket,
            _input,
            '\n',
            [self = shared_from_this(),
             next](const boost::system::error_code& error, const std::size_t) {
                if (self->_closed)
                    return;
                self->_cancelTimer();
                if (error) {
                    self->_fail(lld::exception::et_read_error, error.message());
                    return;
                }
                std::istream input(&self->_input);
                std::string reply;
                std::getline(input, reply);
                lld::detail::trim_reply(reply);
                next(reply);
            }
    );
}


void io_session::_armTimer(const double seconds, const lld::exception::error_type_t error_type)
{
    const auto generation = ++_timerGeneration;
    _timer.expires_after(
            std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                    std::chrono::duration<double>(seconds)
            )
    );
    _timer.async_wait([self = shared_from_this(),
                       generation,
                       error_type](const boost::system::error_code& error) {
        if (!error && !self->_closed && generation == self->_timerGeneration)
            self->_fail(error_type, "Timed out waiting for lockd");
    });
}


void io_session::_cancelTimer()
{
    ++_timerGeneration;
    _timer.cancel();
}


void io_session::_fail(const lld::exception::error_type_t error_type, const std::string& message)
{
    close();
    _notify(false, error_type, message);
}


void io_session::_notify(
        const bool success,
        const lld::exception::error_type_t error_type,
        const std::string& message,
        const bool acquired
)
{
    bool posted = false;
    {
        const gil_acquire lock;
        const py::object loop(py::handle<>(py::borrowed(_loop->get())));
        try {
            if (!py::extract<bool>(loop.attr("is_closed")())) {
                loop.attr("call_soon_threadsafe")(
                        completion{_target, _ticket, success, error_type, message, acquired}
                );
                posted = true;
            }
        }
        catch (const py::error_already_set&) {
            // Closing an event loop can race with a network completion. Other bridge
            // failures are reported rather than silently losing an active operation.
            PyObject* type;
            PyObject* value;
            PyObject* traceback;
            PyErr_Fetch(&type, &value, &traceback);
            const bool closed = py::extract<bool>(loop.attr("is_closed")());
            PyErr_Restore(type, value, traceback);
            if (closed)
                PyErr_Clear();
            else
                PyErr_WriteUnraisable(loop.ptr());
        }
    }
    if (!posted)
        close();
}

void io_runtime::stop()
{
    if (_stopped.exchange(true))
        return;
    boost::asio::post(_service, [this] {
        // close() removes its registry entry. Snapshot strong references first so that closing
        // the sessions cannot invalidate this traversal or destroy a session midway through it.
        std::vector<std::shared_ptr<io_session>> sessions;
        for (const auto& [fst, snd] : _sessions)
            if (auto session = snd.lock())
                sessions.push_back(std::move(session));
        for (const auto& session : sessions)
            session->close();
        _work.reset();
    });
    _thread.join();
}

namespace {

void shutdown_async()
{
    ASYNC_SHUTDOWN = true;
    auto runtime = std::move(IO_RUNTIME);
    if (runtime) {
        // The worker may be acquiring the GIL to deliver its last completion. Joining while
        // holding it would deadlock interpreter shutdown.
        PyThreadState* const state = PyEval_SaveThread();
        runtime->stop();
        runtime.reset();
        PyEval_RestoreThread(state);
    }
}

void cancel_future(const py::object& future)
{
    // Dropping the last scope after its event loop has closed must still close the socket.
    // Future.cancel() would try to schedule callbacks on that loop and throw from a destructor.
    if (!py::extract<bool>(future.attr("get_loop")().attr("is_closed")()))
        future.attr("cancel")();
}


} // namespace

async_mutex::async_mutex(
        std::string hostname,
        const std::uint16_t port,
        const std::int64_t connect_timeout,
        const std::string& name,
        const bool recursive
)
        : _hostname(std::move(hostname))
        , _port(port)
        , _name(name)
        , _connectTimeout(connect_timeout)
        , _recursive(recursive)
{
    lld::detail::validate_lock_name(name);
    lld::detail::validate_connect_timeout(_connectTimeout);
}


async_mutex::~async_mutex()
{
    if (_session)
        _session->cancel();
}


ticket_t async_mutex::new_owner()
{
    return ++_nextOwner;
}


bool async_mutex::owns(const ticket_t owner) const
{
    return _held && (_recursive ? _claims.find(owner) != _claims.end() : _owner == owner);
}


std::pair<py::object, ticket_t> async_mutex::begin(
        const ticket_t owner,
        const operation_type_t type,
        const py::object& scope,
        const py::object& primary_error,
        const double wait_milliseconds
)
{
    if (ASYNC_SHUTDOWN)
        logic_error("pylockd's async runtime has shut down");
    const py::object loop = py::import("asyncio").attr("get_running_loop")();
    if (_loop.is_none())
        _loop = loop;
    if (_loop.ptr() != loop.ptr())
        logic_error("AsyncMutex cannot be shared between event loops");
    if (type == op_connect) {
        // Awaiting construction establishes the connection once, matching synchronous C++
        // construction. A failed or canceled session stays disconnected rather than reconnecting
        // implicitly when the same mutex is awaited again.
        if (_connectionStarted) {
            if (!_connected)
                throw lld::exception(lld::exception::et_not_connected, "Not connected to lockd.");
            const py::object ready = loop.attr("create_future")();
            ready.attr("set_result")(scope);
            return {ready, 0};
        }
        _connectionStarted = true;
        const py::object future = loop.attr("create_future")();
        const ticket_t ticket = ++_nextTicket;
        const py::object weak_self(py::handle<>(PyWeakref_NewRef(scope.ptr(), nullptr)));
        _active = std::make_unique<request>(
                request{ticket,
                        0,
                        op_connect,
                        future,
                        weak_self,
                        py::object(),
                        0,
                        py::object(),
                        py::object()}
        );
        future.attr("add_done_callback")(cancel_callback{shared_from_this(), ticket});
        _session = std::make_shared<io_session>(
                get_runtime(),
                loop,
                shared_from_this(),
                ticket,
                _hostname,
                _port,
                _name,
                _connectTimeout
        );
        _session->start();
        return {future, ticket};
    }
    if (!_connected)
        throw lld::exception(lld::exception::et_not_connected, "Not connected to lockd.");
    const py::object task =
            _recursive ? py::import("asyncio").attr("current_task")() : py::object();
    if (_recursive && task.is_none())
        logic_error("Recursive async locks require an asyncio task");
    if (is_acquisition(type)) {
        if (owns(owner) || (_active && _active->owner == owner))
            logic_error("This ScopedLock already owns the lock");
        for (const auto& pending : _queue)
            if (pending.owner == owner)
                logic_error("AsyncScopedLock already has a pending acquisition");
    }
    if ((type == op_unlock || type == op_exit) && !owns(owner))
        logic_error("This ScopedLock does not own the lock");
    if (_recursive && owns(owner) && _owningTask().ptr() != task.ptr())
        logic_error("This asyncio task does not own the recursive mutex");

    // Only scopes from the owning task may share this token. A native recursive mutex would
    // treat every task on this event-loop thread as its owner and break task exclusion.
    // shared_ptr owns the lifetime; no second recursion count can drift away from the scopes.
    if (_recursive && is_acquisition(type) && !_claims.empty() &&
        _owningTask().ptr() == task.ptr()) {
        const ticket_t ticket = ++_nextTicket;
        _claims.emplace(owner, recursive_claim{ticket, _claims.begin()->second.ownership});
        const py::object ready = loop.attr("create_future")();
        if (type == op_enter)
            ready.attr("set_result")(scope);
        else if (type == op_try_lock || type == op_timed_lock)
            ready.attr("set_result")(true);
        else
            ready.attr("set_result")(py::object());
        return {ready, ticket};
    }
    if (_recursive && (type == op_unlock || type == op_exit)) {
        const auto claim = _claims.find(owner);
        const bool final = claim->second.ownership.use_count() == 1;
        _claims.erase(claim);
        if (!final) {
            const py::object ready = loop.attr("create_future")();
            ready.attr("set_result")(py::object());
            return {ready, 0};
        }
    }
    if (type == op_try_lock && (_owner || !_queue.empty())) {
        // Local contention is still contention. TRY_LOCK must return promptly without queueing
        // behind another scope or overtaking scopes which are already waiting on this mutex.
        const py::object ready = loop.attr("create_future")();
        ready.attr("set_result")(false);
        return {ready, 0};
    }
    const py::object future = loop.attr("create_future")();
    const py::object weak_scope(py::handle<>(PyWeakref_NewRef(scope.ptr(), nullptr)));
    const ticket_t ticket = ++_nextTicket;
    const py::object task_reference =
            _recursive ? py::object(py::handle<>(PyWeakref_NewRef(task.ptr(), nullptr)))
                       : py::object();

    // A timed request waiting behind another local owner has not reached the daemon yet, so
    // it needs a loop timer. Once sent, the daemon alone owns its remote acquisition deadline.
    const request pending{
            ticket,
            owner,
            type,
            future,
            weak_scope,
            primary_error,
            wait_milliseconds,
            task_reference,
            type == op_timed_lock && _owner
                    ? loop.attr("call_later")(
                              wait_milliseconds / 1000.0,
                              queue_timeout_callback{shared_from_this(), ticket}
                      )
                    : py::object()
    };
    future.attr("add_done_callback")(cancel_callback{shared_from_this(), pending.ticket});
    if (is_acquisition(type)) {
        _queue.push_back(pending);
        _startNext();
    }
    else {
        _active = std::make_unique<request>(pending);
        _held = false;
        _session->release(pending.ticket);
    }
    return {future, pending.ticket};
}


void async_mutex::cancel(const ticket_t ticket)
{
    // A queued request hasn't touched the socket. Remove it without disturbing the scope
    // currently holding the remote lock or any other scope waiting on the same connection.
    for (auto it = _queue.begin(); it != _queue.end(); ++it) {
        if (it->ticket == ticket) {
            const py::object future = it->future;
            if (!it->timer.is_none())
                it->timer.attr("cancel")();
            _queue.erase(it);
            cancel_future(future);
            return;
        }
    }
    if (_recursive) {
        for (auto claim = _claims.begin(); claim != _claims.end(); ++claim) {
            if (claim->second.ticket == ticket) {
                // Cancelling one completed nested acquisition must preserve the other claims,
                // including an outer scope which will unwind after this exception.
                const bool final = claim->second.ownership.use_count() == 1;
                _claims.erase(claim);
                if (final) {
                    _discard();
                    _startNext();
                }
                return;
            }
        }
    }
    if ((_active && _active->ticket == ticket) ||
        (!_recursive && _owner && _ownershipTicket == ticket)) {
        // The server may already have granted this request. Closing the connection relinquishes
        // that possible ownership; canceling only the Future would leave the remote lock held.
        py::object future;
        if (_active)
            future = _active->future;
        _discard();
        if (!future.is_none())
            cancel_future(future);
        _startNext();
    }
}


void async_mutex::expire_queued(const ticket_t ticket)
{
    for (auto it = _queue.begin(); it != _queue.end(); ++it) {
        if (it->ticket == ticket) {
            const py::object future = it->future;
            if (!it->timer.is_none())
                it->timer.attr("cancel")();
            _queue.erase(it);
            if (!py::extract<bool>(future.attr("done")()))
                future.attr("set_result")(false);
            return;
        }
    }
}


void queue_timeout_callback::call() const
{
    if (const auto mutex = target.lock())
        mutex->expire_queued(ticket);
}


void async_mutex::abandon(const ticket_t owner)
{
    for (auto it = _queue.begin(); it != _queue.end();) {
        if (it->owner == owner) {
            const py::object future = it->future;
            if (!it->timer.is_none())
                it->timer.attr("cancel")();
            it = _queue.erase(it);
            cancel_future(future);
        }
        else
            ++it;
    }
    if (_active && _active->owner == owner) {
        cancel(_active->ticket);
        return;
    }
    if (!owns(owner))
        return;
    if (_recursive) {
        const auto claim = _claims.find(owner);
        const bool final = claim->second.ownership.use_count() == 1;
        _claims.erase(claim);
        if (!final)
            return;
    }
    if (ASYNC_SHUTDOWN || py::extract<bool>(_loop.attr("is_closed")())) {
        _discard();
        return;
    }
    const ticket_t ticket = ++_nextTicket;
    _active = std::make_unique<request>(
            request{ticket,
                    owner,
                    op_unlock,
                    py::object(),
                    py::object(),
                    py::object(),
                    0,
                    py::object(),
                    py::object()}
    );
    _held = false;
    _session->release(ticket);
}


void async_mutex::complete(const completion& result)
{
    // Cancellation can retire a request while its worker completion is already queued on the
    // loop. Ignore that old ticket rather than granting ownership to the current request.
    if (!_active || _active->ticket != result.ticket)
        return;
    const request pending = *_active;
    if (pending.future.is_none()) {
        // Destructor-initiated unlock has no Python waiter, but still must acknowledge release
        // before allowing the next queued scope to use this persistent connection.
        if (result.success)
            _releaseOwnership();
        else
            _discard();
        _startNext();
        return;
    }
    if (!pending.future.is_none() && py::extract<bool>(pending.future.attr("cancelled")())) {
        cancel(result.ticket);
        return;
    }
    if (!result.success) {
        py::object error = make_error(result.error_type, result.message);
        _discard();
        if (!pending.primary_error.is_none()) {
            Py_INCREF(error.ptr());
            PyException_SetCause(pending.primary_error.ptr(), error.ptr());
            error = pending.primary_error;
        }
        pending.future.attr("set_exception")(error);
        _startNext();
    }
    else if (is_acquisition(pending.type) && !result.acquired) {
        _releaseOwnership();
        pending.future.attr("set_result")(false);
        _startNext();
    }
    else if (pending.type == op_connect) {
        _connected = true;
        _active.reset();
        const py::object mutex = pending.scope();
        if (mutex.is_none()) {
            _discard();
            pending.future.attr("cancel")();
        }
        else
            pending.future.attr("set_result")(mutex);
    }
    else if (is_acquisition(pending.type)) {
        _held = true;
        _ownershipTicket = pending.ticket;
        if (_recursive) {
            _owningTask = pending.task;
            _claims.emplace(
                    pending.owner,
                    recursive_claim{pending.ticket, std::make_shared<recursive_ownership>()}
            );
        }
        _active.reset();

        // Calling a weak reference gives us a strong reference for set_result(), and
        // works across Python 3.10+ without the deprecated borrowed-reference C API.
        const py::object scope = pending.scope();
        if (scope.is_none()) {
            cancel(pending.ticket);
            pending.future.attr("cancel")();
        }
        else if (pending.type == op_enter)
            pending.future.attr("set_result")(scope);
        else if (pending.type == op_try_lock || pending.type == op_timed_lock)
            pending.future.attr("set_result")(true);
        else
            pending.future.attr("set_result")(py::object());
    }
    else {
        // Release clears ownership, not the connection. Start the next local waiter only after
        // the daemon acknowledges UNLOCK, so commands and replies cannot cross scope boundaries.
        _releaseOwnership();
        pending.future.attr("set_result")(py::object());
        _startNext();
    }
}


void async_mutex::_releaseOwnership()
{
    _active.reset();
    _claims.clear();
    _owningTask = py::object();
    _owner = 0;
    _held = false;
}


void async_mutex::_discard()
{
    if (_session)
        _session->cancel();
    _session.reset();
    _connected = false;
    _releaseOwnership();
}


void async_mutex::_startNext()
{
    if (_owner || _queue.empty())
        return;
    const request pending = _queue.front();
    _queue.pop_front();
    if (!pending.timer.is_none())
        pending.timer.attr("cancel")();
    if (!pending.future.is_none() && py::extract<bool>(pending.future.attr("cancelled")())) {
        _startNext();
        return;
    }
    _owner = pending.owner;
    _ownershipTicket = pending.ticket;
    _active = std::make_unique<request>(pending);
    if (!_connected) {
        complete(
                completion{
                        shared_from_this(),
                        pending.ticket,
                        false,
                        lld::exception::et_not_connected,
                        "Not connected to lockd.",
                        false
                }
        );
        return;
    }
    _session->request_lock(
            pending.ticket,
            pending.type == op_try_lock,
            pending.type == op_timed_lock
                    ? lld::detail::lock_wait_milliseconds(pending.wait_milliseconds)
                    : std::chrono::milliseconds::zero()
    );
}

void completion::call() const
{
    if (const auto mutex = target.lock())
        mutex->complete(*this);
}

void cancel_callback::call(const py::object& future) const
{
    if (py::extract<bool>(future.attr("cancelled")()))
        if (const auto mutex = target.lock())
            mutex->cancel(ticket);
}

async_scoped_lock::async_scoped_lock(async_mutex& mutex)
        : _mutex(mutex.shared_from_this())
        , _owner(_mutex->new_owner())
{
}


async_scoped_lock::~async_scoped_lock()
{
    const python_error_guard error;
    _mutex->abandon(_owner);
}


bool async_scoped_lock::owns_lock() const
{
    return _mutex->owns(_owner);
}


std::pair<py::object, ticket_t> async_scoped_lock::begin(
        const operation_type_t type,
        const py::object& self,
        const py::object& primary_error,
        const double wait_milliseconds
) const
{
    return _mutex->begin(_owner, type, self, primary_error, wait_milliseconds);
}


void async_scoped_lock::cancel(const ticket_t ticket) const
{
    _mutex->cancel(ticket);
}

async_timed_mutex::async_timed_mutex(
        const std::string& hostname,
        const std::uint16_t port,
        const std::int64_t connect_timeout,
        const std::string& name
)
        : async_mutex(hostname, port, connect_timeout, name)
{
}


async_timed_mutex::~async_timed_mutex() = default;


async_scoped_timed_lock::async_scoped_timed_lock(async_timed_mutex& mutex)
        : async_scoped_lock(mutex)
{
}


async_scoped_timed_lock::~async_scoped_timed_lock() = default;


async_recursive_mutex::async_recursive_mutex(
        const std::string& hostname,
        const std::uint16_t port,
        const std::int64_t connect_timeout,
        const std::string& name
)
        : async_mutex(hostname, port, connect_timeout, name, true)
{
}


async_recursive_mutex::~async_recursive_mutex() = default;


async_scoped_recursive_lock::async_scoped_recursive_lock(async_recursive_mutex& mutex)
        : async_scoped_lock(mutex)
{
}


async_scoped_recursive_lock::~async_scoped_recursive_lock() = default;


async_operation::async_operation(
        py::object scope,
        const operation_type_t type,
        py::object primary_error,
        const double wait_value,
        const bool absolute_deadline
)
        : _scope(std::move(scope))
        , _type(type)
        , _waitValue(wait_value)
        , _absoluteDeadline(absolute_deadline)
        , _primaryError(std::move(primary_error))
{
}


async_operation::~async_operation()
{
    const python_error_guard error;
    close();
}


const async_operation& async_operation::await() const
{
    return *this;
}


py::object async_operation::next()
{
    return send(py::object());
}


py::object async_operation::send(const py::object& value)
{
    _start();
    try {
        return _iterator.attr("send")(value);
    }
    catch (const py::error_already_set&) {
        const bool failed = !PyErr_ExceptionMatches(PyExc_StopIteration);

        // Future.send() leaves the Python exception pending, including normal
        // StopIteration. Cleanup can destroy a scope and start its next waiter, so those
        // Python calls must run with a clear error indicator before we restore this one.
        const python_error_guard error;
        _finished = true;
        if (failed)
            _cancel();
        _clearReferences();
        throw;
    }
}


py::object async_operation::throw_into(const py::tuple& arguments)
{
    _start();

    // Task.cancel() can arrive after the grant's Future was completed but before the
    // coroutine resumed. Cancel the ticket synchronously before propagating the exception.
    _cancel();
    _finished = true;
    const py::object throw_method = _iterator.attr("throw");
    _clearReferences();
    return py::object(py::handle<>(PyObject_CallObject(throw_method.ptr(), arguments.ptr())));
}


void async_operation::close()
{
    if (_finished)
        return;
    _finished = true;
    if (_started)
        _cancel();
    _clearReferences();
}


void async_operation::_clearReferences()
{
    // Completed Python coroutines release their frames. Do the same for our references:
    // retaining an old awaitable must not keep a scope/socket alive, or form an invisible
    // C++ -> exception traceback -> Python frame -> awaitable reference cycle.
    _future = py::object();
    _iterator = py::object();
    _primaryError = py::object();
    _scope = py::object();
}


const async_scoped_lock& async_operation::_scopeLock() const
{
    return py::extract<const async_scoped_lock&>(_scope);
}


void async_operation::_cancel() const
{
    if (_type == op_connect) {
        async_mutex& mutex = py::extract<async_mutex&>(_scope);
        mutex.cancel(_ticket);
    }
    else
        _scopeLock().cancel(_ticket);
}


void async_operation::_start()
{
    if (_finished)
        logic_error("Cannot reuse an already awaited lock operation");
    if (!_started) {
        _started = true;

        // Coroutine creation and first execution can be separated by arbitrary user work.
        // Resolve absolute deadlines here so retaining an unstarted operation cannot extend
        // its deadline. Relative durations start when the operation is first awaited.
        const double wait_milliseconds =
                _absoluteDeadline ? (_waitValue - system_seconds()) * 1000.0 : _waitValue;
        if (_type == op_timed_lock)
            static_cast<void>(lld::detail::lock_wait_milliseconds(wait_milliseconds));
        const operation_type_t type =
                _type == op_timed_lock && wait_milliseconds <= 0 ? op_try_lock : _type;
        const auto [fst, snd] =
                _type == op_connect
                        ? py::extract<async_mutex&>(_scope)().begin(
                                  0, op_connect, _scope, _primaryError, 0
                          )
                        : _scopeLock().begin(type, _scope, _primaryError, wait_milliseconds);
        _ticket = snd;
        _future = fst;
        _iterator = _future.attr("__await__")();
    }
}

namespace {

py::object operation_throw(const py::tuple& arguments, const py::dict& keywords)
{
    if (py::len(keywords) || py::len(arguments) < 2 || py::len(arguments) > 4)
        logic_error("throw expects an exception and optional value/traceback");
    const py::tuple forwarded(
            py::handle<>(PyTuple_GetSlice(arguments.ptr(), 1, py::len(arguments)))
    );
    return py::extract<async_operation&>(arguments[0])().throw_into(forwarded);
}

py::object mutex_await(const py::object& self)
{
    return py::object(std::make_shared<async_operation>(self, op_connect, py::object()));
}


py::object scope_lock(const py::object& self)
{
    return py::object(std::make_shared<async_operation>(self, op_lock, py::object()));
}

py::object scope_enter(const py::object& self)
{
    return py::object(std::make_shared<async_operation>(self, op_enter, py::object()));
}

py::object scope_unlock(const py::object& self)
{
    return py::object(std::make_shared<async_operation>(self, op_unlock, py::object()));
}


py::object scope_try_lock(const py::object& self)
{
    return py::object(std::make_shared<async_operation>(self, op_try_lock, py::object()));
}


py::object timed_operation(const py::object& self, const double wait_milliseconds)
{
    static_cast<void>(lld::detail::lock_wait_milliseconds(wait_milliseconds));

    // As in C++, positive waits go to the daemon; nonpositive waits make one TRY_LOCK attempt.
    const operation_type_t type = wait_milliseconds > 0 ? op_timed_lock : op_try_lock;
    return py::object(
            std::make_shared<async_operation>(self, type, py::object(), wait_milliseconds)
    );
}


py::object scope_try_lock_for(const py::object& self, const double milliseconds)
{
    return timed_operation(self, milliseconds);
}


py::object scope_try_lock_until(const py::object& self, const double abs_time)
{
    static_cast<void>(lld::detail::lock_wait_milliseconds((abs_time - system_seconds()) * 1000.0));
    return py::object(
            std::make_shared<async_operation>(self, op_timed_lock, py::object(), abs_time, true)
    );
}

py::object
scope_exit(const py::object& self, const py::object&, const py::object& error, const py::object&)
{
    return py::object(std::make_shared<async_operation>(self, op_exit, error));
}

} // namespace

} // namespace pld::detail

void pld::export_async_types()
{
    using namespace pld::detail;

    // @formatter:off
    py::class_<completion>("_AsyncCompletion", py::no_init).def("__call__", &completion::call);
    py::class_<cancel_callback>("_AsyncCancel", py::no_init)
            .def("__call__", &cancel_callback::call);
    py::class_<queue_timeout_callback>("_AsyncQueueTimeout", py::no_init)
            .def("__call__", &queue_timeout_callback::call);
    py::class_<async_operation, std::shared_ptr<async_operation>, boost::noncopyable>(
            "_AsyncOperation", py::no_init
    )
            .def("__await__", &async_operation::await, py::return_self<>())
            .def("__iter__", &async_operation::await, py::return_self<>())
            .def("__next__", &async_operation::next)
            .def("send", &async_operation::send)
            .def("throw", py::raw_function(&operation_throw))
            .def("close", &async_operation::close);
    const py::object operation_type = py::scope().attr("_AsyncOperation");
    py::import("collections.abc").attr("Coroutine").attr("register")(operation_type);
    // Construction registers the Python type; no C++ guard lifetime is required.
    // NOLINTNEXTLINE(bugprone-unused-raii)
    py::class_<async_mutex, std::shared_ptr<async_mutex>, boost::noncopyable>(
            "AsyncMutex",
            "Await construction to connect. The connection persists between lock scopes. "
            "Use one event loop; cancellation of in-flight I/O invalidates the connection.",
            py::init<const std::string&, std::uint16_t, std::int64_t, const std::string&>(
                    (py::arg("hostname"),
                     py::arg("port"),
                     py::arg("connect_timeout"),
                     py::arg("lock_name"))
            )
    )
            .def("__await__", &mutex_await);
    py::class_<async_scoped_lock, std::shared_ptr<async_scoped_lock>, boost::noncopyable>(
            "AsyncScopedLock",
            "Use async with to acquire and release the mutex. "
            "The scope retains its mutex. Cancellation discards the connection; cleanup "
            "failures are chained without replacing a body exception or cancellation.",
            py::init<async_mutex&>()
    )
            .def("lock", &scope_lock)
            .def("try_lock",
                 &scope_try_lock,
                 "Attempt acquisition without queueing for local or remote contention.")
            .def("unlock", &scope_unlock)
            .def("owns_lock", &async_scoped_lock::owns_lock)
            .def("__aenter__", &scope_enter)
            .def("__aexit__", &scope_exit);
    // Construction registers the Python type; no C++ guard lifetime is required.
    // NOLINTNEXTLINE(bugprone-unused-raii)
    py::class_<
            async_timed_mutex,
            std::shared_ptr<async_timed_mutex>,
            py::bases<async_mutex>,
            boost::noncopyable>(
            "AsyncTimedMutex",
            "Await construction to connect a timed mutex.",
            py::init<const std::string&, std::uint16_t, std::int64_t, const std::string&>(
                    (py::arg("hostname"),
                     py::arg("port"),
                     py::arg("connect_timeout"),
                     py::arg("lock_name"))
            )
    );
    py::class_<
            async_scoped_timed_lock,
            std::shared_ptr<async_scoped_timed_lock>,
            py::bases<async_scoped_lock>,
            boost::noncopyable>(
            "AsyncScopedTimedLock",
            "Use async with to acquire and release a timed async mutex. "
            "Context entry waits until the lock is acquired.",
            py::init<async_timed_mutex&>()
    )
            .def("timed_lock", &scope_lock)
            .def("try_lock_for",
                 &scope_try_lock_for,
                 "Acquire within a duration in milliseconds; return a boolean.")
            .def("try_lock_until",
                 &scope_try_lock_until,
                 "Acquire by a Unix timestamp in seconds; return a boolean.");
    // Construction registers the Python type; no C++ guard lifetime is required.
    // NOLINTNEXTLINE(bugprone-unused-raii)
    py::class_<
            async_recursive_mutex,
            std::shared_ptr<async_recursive_mutex>,
            py::bases<async_mutex>,
            boost::noncopyable>(
            "AsyncRecursiveMutex",
            "Await construction to connect. Distinct scopes in the owning asyncio task may "
            "reenter; other tasks remain excluded until all those scopes release. Recursion "
            "is per mutex instance, and each async with block must run in the owning task.",
            py::init<const std::string&, std::uint16_t, std::int64_t, const std::string&>(
                    (py::arg("hostname"),
                     py::arg("port"),
                     py::arg("connect_timeout"),
                     py::arg("lock_name"))
            )
    );
    // Construction registers the Python type; no C++ guard lifetime is required.
    // NOLINTNEXTLINE(bugprone-unused-raii)
    py::class_<
            async_scoped_recursive_lock,
            std::shared_ptr<async_scoped_recursive_lock>,
            py::bases<async_scoped_lock>,
            boost::noncopyable>(
            "AsyncScopedRecursiveLock",
            "An acquisition of a task-recursive remote mutex.",
            py::init<async_recursive_mutex&>()
    );
    py::def("_shutdown_async", &shutdown_async);
    const py::object shutdown = py::scope().attr("_shutdown_async");
    py::import("atexit").attr("register")(shutdown);
    // @formatter:on
}
