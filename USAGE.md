# Using the client libraries

Clients coordinate through a daemon and a shared lock name. Each connection can hold or wait for one
remote lock at a time. Prefer scope guards so exceptions and ordinary returns release the lock. Use
separate client instances for independent contenders, even when they are in the same process.

## Thread safety

**Do not share mutex or lock objects between threads.** Give each thread its own mutex, connection,
and lock guard. Use the same lock name when those threads need to protect the same resource.

The remote lock protects your shared resource; it does not make the client objects thread-safe. Keep
acquisition, use, release, and cleanup on the owning thread. The protocol also expects one caller
per connection: don't have multiple threads send commands or read replies on the same connection.

## Lock names

Names are case-sensitive and must match `[a-zA-Z0-9_-]{1,256}`: 1-256 ASCII letters, digits,
underscores, or hyphens. Non-ASCII names are not supported.

## C++ locks

All constructors take the daemon hostname, port, connection timeout in `std::chrono::seconds`, and
lock name. The `lock_name()` setter applies the same validation and preserves the previous name when
rejected. Include the corresponding header under `liblockd/`. Constructors reject invalid names with
`lld::exception::et_logic_error` before connecting.

| Type                   | Operations                                                    |
|------------------------|---------------------------------------------------------------|
| `lld::mutex`           | `lock()`, `try_lock()`, `unlock()`                            |
| `lld::timed_mutex`     | Those operations plus `try_lock_for()` and `try_lock_until()` |
| `lld::recursive_mutex` | Balanced recursive acquisition by the owning thread           |

`lock()` waits for ownership. `try_lock()` makes one immediate remote attempt and returns whether it
acquired the lock; it still requires network I/O. `unlock()` releases ownership. The `scoped_lock`
aliases use `std::unique_lock`, so `std::defer_lock`, `std::try_to_lock`, and `std::adopt_lock` work
as usual. Only adopt a lock you already own.

```cpp
#include <chrono>
#include <mutex>
#include <liblockd/timed_mutex.hpp>

int main()
{
    lld::timed_mutex mutex("localhost", 7198, std::chrono::seconds(5), "my_lock");
    std::unique_lock<lld::timed_mutex> lock(mutex, std::defer_lock);
    if (lock.try_lock_for(std::chrono::seconds(2))) {
        // Work protected by my_lock.
    }
}
```

`recursive_mutex` tracks recursion by the owning thread. Recursion belongs to that instance: another
instance using the same remote name still competes for the lock. Balance every successful
acquisition with a release on the owning thread.

## Python locks

The Python interface closely follows the C++ interface, with ordinary, timed, and recursive locks.
It supports both synchronous context managers (`with`) and asyncio context managers (`async with`).
See [PYTHON.md](PYTHON.md) for examples, the available methods, time units, and errors.

## Time units and deadlines

Relative C++ timeouts accept `std::chrono` durations with millisecond or coarser resolution.
`std::chrono::nanoseconds` and `microseconds` are rejected by `try_lock_for` at compile time.
Positive fractional milliseconds in accepted duration types are rounded up to a whole millisecond.

`try_lock_until` accepts a clock's native time-point precision and rounds a positive remaining wait
up to milliseconds. Use `std::chrono::steady_clock` for an elapsed-time deadline when wall-clock
adjustments shouldn't affect it.

A caller's explicit `std::chrono::duration_cast` to an integral unit truncates toward zero: casting
500 nanoseconds to milliseconds produces zero. That happens before lockd sees the value. Nonpositive
durations and expired deadlines make an immediate remote `try_lock` attempt; they don't skip the
attempt. These are lock wait budgets, not a promise of a zero-time network operation.

Connection timeouts are separate from lock acquisition timeouts. The convenience `timed_lock` C++
overloads forward to the duration/deadline methods.

## Connection failures

lockd keeps locks in memory. It is not intended to provide database-grade durability or fault
tolerance. A daemon crash, forced termination, machine failure, or lost connection can interrupt an
acquisition or lose a held lock. Restarting the daemon starts with no locks; it does not restore
ownership from the previous process.

Clients are responsible for acquisition failures and dropped connections, including catching and
logging exceptions and deciding whether their work can safely continue or be retried. Acquiring a
lock does not guarantee that the connection or daemon stays alive throughout the protected work.

Native exceptions report source filenames relative to the lockd repository root.

The synchronous C++ library reports unexpected disconnections as `lld::exception` when I/O next
observes them. `connected()` reports the last known connection state; it is not a liveness probe.
Cleanup destructors and the default `unlock()` suppress exceptions; use
`unlock(lld::mutex::tt_throw_on_failure)` to check an explicit release. The client application owns
logging; the client library does not print errors automatically.

A command-reply timeout or socket read/write failure closes the connection and raises a client
exception. Create a new mutex before retrying; the failed connection is not reused. A
daemon-reported lock acquisition timeout is ordinary contention and leaves the connection available
for another attempt.

In Python, catch `pylockd.Exception` (or the built-in `Exception`). Translated native errors expose
`.what` and a `.cause` with `error_type()`, `file()`, `line()`, and `function()` details.

### Coordinating a persistent update

Use lockd to serialize the usual path, and let persistent storage decide whether an update is valid.
A connection can fail after acquisition, allowing another client to acquire the same lock while the
first client is still doing work. Checking `connected()` before writing cannot close that race.

For example, a reservation needs an atomic storage operation that checks availability, saves the
reservation, and records its result under a unique **operation ID**. Repeating that ID returns the
recorded result rather than applying the update again. Keep the same ID across retries and
application restarts; a different customer's request must have a different ID.

The following is C++-style pseudocode. The `storage` operations represent your application's
persistent store, with no particular storage technology required:

```cpp
// operation_id identifies this business request and survives retries and restarts.
// resource_id also supplies a valid, shared lock name, such as "seat_42".
for (int attempt = 0; attempt < 3; ++attempt) {
    auto outcome = unknown;
    try {
        lld::mutex mutex("lockd.internal", 7198, std::chrono::seconds(5), resource_id);
        const std::lock_guard<lld::mutex> lock(mutex);

        // Atomically: return this operation's recorded result if it exists; otherwise
        // check availability, commit at most one reservation, and record the outcome.
        // A competing operation that already reserved the resource produces a conflict.
        outcome = storage.commit_once_if_available(resource_id, operation_id, request);
    }
    catch (const lld::exception& error) {
        log(error.what());
    }

    // A lost storage reply can leave a successful commit looking like a failure.
    // Query the recorded result; an absent result or unavailable store remains unknown.
    if (outcome == unknown)
        outcome = storage.lookup_operation_result(operation_id);
    if (outcome != unknown)
        return outcome; // Committed, or definitively rejected because another request won.

    wait_before_retry(attempt);
}

// Preserve the ID for later reconciliation. Unknown does not mean the update failed.
return pending(operation_id);
```

Here, storage timeouts and connection failures produce `unknown`; ordinary validation failures
produce a definite rejection. If the first commit is still in progress when a retry starts, the
storage operation must still enforce the same operation ID and availability check atomically. A
separate "check, then write" is not sufficient. Use your storage's transactions, conditional writes,
version checks, or uniqueness guarantees to enforce that contract.

Once storage confirms the commit, a failed lock release does not undo it. If the application also
calls an external service, that effect needs its own retry protection, such as an idempotency key.
The application's durable record determines the result; reconnecting to lockd does not recover the
previous lock or prove what happened to the update.

## Other languages

The [C interface guide](DLL.md) has examples for C, C#, VB.NET, Node.js, and Rust, along with
library setup and handle ownership rules.
