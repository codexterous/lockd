# Using pylockd

lockd is a daemon that provides named locks for coordinating access to shared resources across
processes and machines. See the [main
README.md](https://github.com/codexterous/lockd/blob/main/README.md) for an overview, installation
instructions, and more information about lockd.

pylockd provides synchronous and asyncio interfaces to lockd. Use `with` in ordinary Python code and
`async with` in async code. Both offer ordinary, timed, and recursive locks; a daemon must be
running locally or on another machine.

## Install

Install pylockd from PyPI:

```sh
python -m pip install pylockd
```

You can also download a wheel matching your Python version and platform, then install it:

```sh
python -m pip install <path-to-wheel>
```

Release wheels support CPython 3.11 through 3.15 on Windows and Linux, each on x86, x86_64, and
ARM64. Linux wheels bundle their Boost dependency; see
[COMPATIBILITY.md](https://github.com/codexterous/lockd/blob/main/COMPATIBILITY.md#portable-linux-wheels).
For source builds and editor setup, see
[CONTRIBUTING.md](https://github.com/codexterous/lockd/blob/main/CONTRIBUTING.md#building-python-from-source).

## Synchronous usage

```python
import pylockd

mutex = pylockd.Mutex(
    hostname='localhost',
    port=7198,
    connect_timeout=5,
    lock_name='my_lock',
)
with pylockd.ScopedLock(mutex):
    print('Do the protected work here.')
```

The mutex connects to the daemon. Its arguments are hostname, port, connection timeout in whole
seconds, and lock name. Ports range from 0 to 65535. Connection timeouts must be nonnegative and fit
the system clock; both interfaces accept the same signed 64-bit integer input. The five-second
timeout applies to connecting, not waiting for the lock. Entering `with` waits for ownership;
leaving it releases the lock, including when the body raises an exception.

Invalid lock names (see [USAGE.md](https://github.com/codexterous/lockd/blob/main/USAGE.md)) raise
`pylockd.Exception` before connecting, for both synchronous and async mutexes.

**Don't share mutexes or lock guards between threads.** Each thread needs its own objects and
connection. Use the same name to protect the same resource. The GIL doesn't make these objects safe
to share.

## Async usage

Use the async types to avoid blocking an asyncio event loop:

```python
import asyncio
import pylockd


async def main():
    mutex = await pylockd.AsyncMutex(
        hostname='localhost',
        port=7198,
        connect_timeout=5,
        lock_name='my_lock',
    )
    async with pylockd.AsyncScopedLock(mutex):
        await asyncio.sleep(0)  # Do the protected asynchronous work.


asyncio.run(main())
```

Await async mutex construction to connect before using a guard. Both interfaces retain their
connection between lock scopes, including after an unsuccessful timed or immediate acquisition.
Reusing a mutex does not reconnect automatically.

Task cancellation raises `asyncio.CancelledError`; don't swallow it and continue as though you
acquired the lock. Canceling an in-flight network operation closes its connection to release any
ownership racing with cancellation. Create a new mutex before trying again. Canceling a local waiter
on a shared mutex leaves the current owner's connection intact.

Keep async objects on one event loop and thread. Each `async with` block must run in the same task
that entered it. Use a separate guard for each scope. Synchronous context managers block the calling
thread; use `async with` inside async functions.

Both constructors take the same four arguments. Protocol read deadlines match the C++ client.

## Available lock types

Pair each mutex with its context manager:

| Mutex                 | Guard                      | Use                           |
|-----------------------|----------------------------|-------------------------------|
| `Mutex`               | `ScopedLock`               | Ordinary blocking acquisition |
| `TimedMutex`          | `ScopedTimedLock`          | Synchronous timed mutex       |
| `RecursiveMutex`      | `ScopedRecursiveLock`      | Reentry by the owning thread  |
| `AsyncMutex`          | `AsyncScopedLock`          | Ordinary asyncio acquisition  |
| `AsyncTimedMutex`     | `AsyncScopedTimedLock`     | Async timed mutex             |
| `AsyncRecursiveMutex` | `AsyncScopedRecursiveLock` | Reentry by the owning task    |

Enter synchronous guards with `with` and async guards with `async with`. Context entry waits until
the lock is acquired, including for timed mutexes. Exiting the context releases it, including when
the body raises an exception. Constructing a guard alone doesn't acquire the lock.

Timed operations use milliseconds for relative waits and Unix timestamps in seconds for absolute
deadlines. Both interfaces round positive waits up to whole milliseconds and let the daemon enforce
the timeout. Nonpositive waits make one immediate attempt. Nonfinite or excessive waits raise
`pylockd.Exception`.

### Recursive acquisition

Recursive mutexes let independently useful methods keep their own locking when one calls another.
For example, a Django shop might reserve stock directly or reserve it while creating an order.
Both methods protect the same product, so the inner method needs to acquire the lock again without
releasing the outer method's protection.

In the shop application's `models.py`:

```python
import pylockd
from django.db import models, transaction
from django.db.models import F


class Product(models.Model):
    stock = models.PositiveIntegerField(default=0)

    def reserve_stock(self, quantity, *, mutex):
        if quantity <= 0:
            raise ValueError('Quantity must be positive.')

        with pylockd.ScopedRecursiveLock(mutex):
            updated = Product.objects.filter(pk=self.pk, stock__gte=quantity).update(
                stock=F('stock') - quantity,
            )
            if updated != 1:
                raise ValueError('Insufficient stock.')

    def create_order(self, quantity, *, mutex):
        with pylockd.ScopedRecursiveLock(mutex), transaction.atomic():
            self.reserve_stock(quantity, mutex=mutex)
            return self.orders.create(quantity=quantity)


class Order(models.Model):
    product = models.ForeignKey(
        Product, on_delete=models.PROTECT, related_name='orders'
    )
    quantity = models.PositiveIntegerField()
```

From a synchronous view or service function, create one mutex and pass it through the operation:

```python
import pylockd
from shop.models import Product


def create_order(product_id, quantity):
    product = Product.objects.get(pk=product_id)
    mutex = pylockd.RecursiveMutex(
        hostname='localhost',
        port=7198,
        connect_timeout=5,
        lock_name=f'product_{product.pk}',
    )
    return product.create_order(quantity, mutex=mutex)
```

`reserve_stock()` can also be called directly with its own mutex and still protects itself. When
called by `create_order()`, it reuses that operation's mutex: leaving the inner guard keeps the
product locked until the outer guard exits. Each request has its own connection; recursion requires
the same mutex object, not just another connection with the same lock name.

The database's [conditional update](https://docs.djangoproject.com/en/5.2/topics/db/queries/#updating-multiple-objects-at-once)
enforces stock availability, and [transaction.atomic()](https://docs.djangoproject.com/en/5.2/topics/db/transactions/#controlling-transactions-explicitly)
keeps the stock change and order creation together.

For asyncio, use `AsyncRecursiveMutex`, `AsyncScopedRecursiveLock`, and `async with`. Nested scopes
must belong to the same task. Other contenders wait until the final scope releases. Another mutex
object with the same lock name is a separate contender, not a recursive acquisition.

## Errors

Validation, lock usage, connection, and protocol errors raise `pylockd.Exception`. Catch it around
the whole operation, including mutex construction:

```python
import pylockd

try:
    mutex = pylockd.Mutex(
        hostname='localhost',
        port=7198,
        connect_timeout=5,
        lock_name='my_lock',
    )
    with pylockd.ScopedLock(mutex):
        print('Do the protected work here.')
except pylockd.Exception as error:
    print(error.what)
```

Native exceptions expose `.what` and `.cause`; the latter provides `error_type()`, `file()`,
`line()`, and `function()` for diagnostics. Exceptions from Python itself and asyncio propagate
unchanged, including call-signature errors, type errors, and `asyncio.CancelledError`. Exceptions
raised by the protected body also propagate unchanged.

Native source filenames are relative to the lockd repository root.

If context cleanup fails, it raises `pylockd.Exception`. When the body has already raised an
exception, that original exception is preserved and the cleanup failure is attached as its cause.
Failed remote release clears the guard's ownership and discards the uncertain connection.

Losing the connection or daemon can invalidate ownership. Decide whether the protected work can
safely continue or be retried; see
[USAGE.md](https://github.com/codexterous/lockd/blob/main/USAGE.md#connection-failures).
