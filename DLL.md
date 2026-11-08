# Using the C interface

`liblockd_dll` exposes lockd through a small C API. Use it from C or languages that can call native
libraries, including C#, VB.NET, Rust, and Node.js. It works with Windows DLLs and Unix shared
libraries; it isn't specific to Windows.

Start a daemon, then use one of the examples below to acquire `my_lock` on `localhost:7198`. Each
example releases the lock before destroying its connection.

**Do not share mutex or lock handles between threads.** Each thread needs its own handles and
connection, using the same lock name to protect the same resource. See [thread
safety](USAGE.md#thread-safety).

## Build and load the library

Build a shared profile using instructions from [BUILDING.md](BUILDING.md). The library is under
`lib/liblockd_dll/output/<arch>/<profile>/shared/`. On Windows, copy `liblockd_dll.dll`,
`liblockd.dll`, and their required runtime DLLs beside your application. Match the library to the
application's architecture, not just the OS. For .NET, select x86 or x64 explicitly instead of
relying on Any CPU defaults.

On Unix, install the SDK and shared libraries into a loader-visible location. The C example below
shows how to compile and link against the shared SDK.

- [C](#c)
- [C# on Windows](#c-on-windows)
- [VB.NET on Windows](#vbnet-on-windows)
- [Rust](#rust)
- [Node.js](#nodejs)

## C

Include `<liblockd_dll/liblockd_dll.h>` from C99 or later, or from C++:

```c
#include <stdio.h>
#include <stdlib.h>
#include <liblockd_dll/liblockd_dll.h>

static void report_error(void)
{
    char message[512] = {0};
    (void)get_last_error(message, sizeof(message));
    fprintf(stderr, "%s\n", message[0] ? message : "lockd operation failed");
}

int main(void)
{
    void* const mutex = create_mutex("localhost", 7198, 5, "my_lock");
    if (!mutex) {
        report_error();
        return EXIT_FAILURE;
    }

    void* const guard = create_lock(mutex);
    if (!guard) {
        report_error();
        destroy_mutex(mutex);
        return EXIT_FAILURE;
    }

    const bool acquired = lock(guard);
    if (acquired) {
        puts("Lock acquired; do the protected work here.");
    }
    else {
        report_error();
    }

    destroy_lock(guard);
    destroy_mutex(mutex);
    return acquired ? EXIT_SUCCESS : EXIT_FAILURE;
}
```

Save this as `example.c` and compile and link against an installed shared SDK on Unix:

```sh
cc -std=c99 example.c -o example \
    -I/path/to/sdk/include -L/path/to/sdk/lib -lliblockd_dll
```

Replace `/path/to/sdk` with the SDK installation prefix, using `lib64` instead of `lib` if needed.
The runtime loader must also find the shared libraries. Static linking also needs the matching C++
runtime and dependencies.

## C# on Windows

This console example uses P/Invoke and `try/finally` to keep both handles scoped. The declarations
use UTF-8 input strings and the native C calling convention.

```csharp
using System;
using System.Runtime.InteropServices;
using System.Text;

internal static class Program
{
    private const string Dll = "liblockd_dll.dll";

    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
    private static extern IntPtr create_mutex(
        [MarshalAs(UnmanagedType.LPUTF8Str)] string server, ushort port, long timeout,
        [MarshalAs(UnmanagedType.LPUTF8Str)] string name);

    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
    private static extern IntPtr create_lock(IntPtr mutex);

    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
    [return: MarshalAs(UnmanagedType.U1)]
    private static extern bool @lock(IntPtr guard);

    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
    private static extern void destroy_lock(IntPtr guard);

    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
    private static extern void destroy_mutex(IntPtr mutex);

    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
    [return: MarshalAs(UnmanagedType.U1)]
    private static extern bool get_last_error([Out] byte[] buffer, UIntPtr capacity);

    private static Exception LastError()
    {
        byte[] buffer = new byte[512];
        get_last_error(buffer, new UIntPtr((uint)buffer.Length));
        int length = Array.IndexOf(buffer, (byte)0);
        string message = Encoding.UTF8.GetString(buffer, 0, length);
        return new InvalidOperationException(
            message.Length == 0 ? "lockd operation failed" : message);
    }

    public static int Main()
    {
        try {
            Run();
            return 0;
        }
        catch (Exception error) {
            Console.Error.WriteLine(error.Message);
            return 1;
        }
    }

    private static void Run()
    {
        IntPtr mutex = create_mutex("localhost", 7198, 5, "my_lock");
        if (mutex == IntPtr.Zero)
            throw LastError();

        try {
            IntPtr guard = create_lock(mutex);
            if (guard == IntPtr.Zero)
                throw LastError();

            try {
                if (!@lock(guard))
                    throw LastError();
                Console.WriteLine("Lock acquired; do the protected work here.");
            }
            finally {
                destroy_lock(guard);
            }
        }
        finally {
            destroy_mutex(mutex);
        }
    }
}
```

## VB.NET on Windows

Use the same matching DLLs as the C# example. Timeout seconds use VB.NET `Long` (64-bit).

```vbnet
Option Strict On
Imports System
Imports System.Runtime.InteropServices
Imports System.Text

Module Program
    Private Const Dll As String = "liblockd_dll.dll"

    <DllImport(Dll, CallingConvention:=CallingConvention.Cdecl, ExactSpelling:=True)>
    Private Function create_mutex(
        <MarshalAs(UnmanagedType.LPUTF8Str)> server As String,
        port As UShort, timeout As Long,
        <MarshalAs(UnmanagedType.LPUTF8Str)> name As String) As IntPtr
    End Function

    <DllImport(Dll, CallingConvention:=CallingConvention.Cdecl, ExactSpelling:=True)>
    Private Function create_lock(mutex As IntPtr) As IntPtr
    End Function

    <DllImport(Dll, CallingConvention:=CallingConvention.Cdecl, ExactSpelling:=True)>
    Private Function lock(guard As IntPtr) As <MarshalAs(UnmanagedType.U1)> Boolean
    End Function

    <DllImport(Dll, CallingConvention:=CallingConvention.Cdecl, ExactSpelling:=True)>
    Private Sub destroy_lock(guard As IntPtr)
    End Sub

    <DllImport(Dll, CallingConvention:=CallingConvention.Cdecl, ExactSpelling:=True)>
    Private Sub destroy_mutex(mutex As IntPtr)
    End Sub

    <DllImport(Dll, CallingConvention:=CallingConvention.Cdecl, ExactSpelling:=True)>
    Private Function get_last_error(<Out> buffer As Byte(), capacity As UIntPtr) _
        As <MarshalAs(UnmanagedType.U1)> Boolean
    End Function

    Private Function LastError() As Exception
        Dim buffer(511) As Byte
        get_last_error(buffer, New UIntPtr(CUInt(buffer.Length)))
        Dim length As Integer = Array.IndexOf(buffer, CByte(0))
        Dim message As String = Encoding.UTF8.GetString(buffer, 0, length)
        Return New InvalidOperationException(
            If(message.Length = 0, "lockd operation failed", message))
    End Function

    Function Main() As Integer
        Try
            Run()
            Return 0
        Catch failure As Exception
            Console.Error.WriteLine(failure.Message)
            Return 1
        End Try
    End Function

    Private Sub Run()
        Dim mutex As IntPtr = create_mutex("localhost", 7198, 5, "my_lock")
        If mutex = IntPtr.Zero Then Throw LastError()

        Try
            Dim guard As IntPtr = create_lock(mutex)
            If guard = IntPtr.Zero Then Throw LastError()

            Try
                If Not lock(guard) Then Throw LastError()
                Console.WriteLine("Lock acquired; do the protected work here.")
            Finally
                destroy_lock(guard)
            End Try
        Finally
            destroy_mutex(mutex)
        End Try
    End Sub
End Module
```

These .NET examples keep native handles inside one synchronous call. Don't let handles escape that
scope or use them after cleanup. A reusable managed binding should own them with `SafeHandle` and
retain the mutex until its lock is destroyed. Don't call blocking acquisition on a UI thread.

## Rust

A C API is a normal FFI boundary for Rust. Keep the raw handles private and give the owning Rust
type a `Drop` implementation. This small example owns one acquired lock and its connection together;
it isn't a full Rust client library.

```rust
use std::ffi::{c_char, c_void, CStr, CString};

#[link(name = "liblockd_dll")]
extern "C" {
    fn create_mutex(server: *const c_char, port: u16, timeout: i64,
                    name: *const c_char) -> *mut c_void;
    fn create_lock(mutex: *mut c_void) -> *mut c_void;
    fn lock(guard: *mut c_void) -> bool;
    fn destroy_lock(guard: *mut c_void);
    fn destroy_mutex(mutex: *mut c_void);
    fn get_last_error(buffer: *mut c_char, capacity: usize) -> bool;
}

fn last_error() -> String {
    let mut buffer = [0u8; 512];

    // The API terminates every nonempty output buffer, including truncated errors.
    unsafe { get_last_error(buffer.as_mut_ptr().cast(), buffer.len()); }
    let end = buffer.iter().position(|&byte| byte == 0).unwrap();
    String::from_utf8_lossy(&buffer[..end]).into_owned()
}

struct HeldLock {
    mutex: *mut c_void,
    guard: *mut c_void,
}

impl HeldLock {
    fn acquire(server: &CStr, port: u16, name: &CStr) -> Result<Self, String> {
        // CStr supplies valid strings; native creation copies them before returning.
        let mutex = unsafe { create_mutex(server.as_ptr(), port, 5, name.as_ptr()) };
        if mutex.is_null() {
            return Err(last_error());
        }
        let guard = unsafe { create_lock(mutex) };
        if guard.is_null() {
            let error = last_error();
            unsafe { destroy_mutex(mutex); }
            return Err(error);
        }
        let held = Self { mutex, guard };
        if !unsafe { lock(held.guard) } {
            return Err(last_error());
        }
        Ok(held)
    }
}

impl Drop for HeldLock {
    fn drop(&mut self) {
        // Both handles are valid and uniquely owned; the lock must be destroyed first.
        unsafe {
            destroy_lock(self.guard);
            destroy_mutex(self.mutex);
        }
    }
}

fn main() -> Result<(), String> {
    let server = CString::new("localhost").unwrap();
    let name = CString::new("my_lock").unwrap();
    let _held = HeldLock::acquire(&server, 7198, &name)?;
    println!("Lock acquired; do the protected work here.");
    Ok(())
}
```

Save as `example.rs`. With the shared SDK installed, compile with Rust 1.64+ (edition 2021):

```sh
rustc --edition=2021 example.rs -L native=/path/to/sdk/lib
```

The runtime loader must also find the shared libraries. On Windows, use an MSVC Rust target matching
the DLL architecture and the directory containing `liblockd_dll.lib`. The raw pointers intentionally
make `HeldLock` neither `Send` nor `Sync`; don't add those traits without designing synchronization
and ownership. Dropping it releases the lock, including during normal panic unwinding.

## Node.js

Node.js 26.1+ can use its built-in [FFI API](https://nodejs.org/api/ffi.html) to call the C library
without an npm binding. This API is experimental and requires a Node.js build with FFI support.
Match the shared library's architecture to the Node.js process.

Save this as `example.cjs`. It uses synchronous calls, so the handles and thread-local error
messages stay on the same thread. The `size_t` declaration follows the process's pointer width.

```javascript
const { dlopen } = require('node:ffi');
const { resolve } = require('node:path');

const sizeType = process.arch === 'ia32' ? 'uint32' : 'uint64';
const port = Number(process.argv[3] ?? 7198);
const { lib, functions } = dlopen(resolve(process.argv[2]), {
    create_mutex: {
        arguments: ['string', 'uint16', 'int64', 'string'],
        return: 'pointer',
    },
    create_lock: { arguments: ['pointer'], return: 'pointer' },
    lock: { arguments: ['pointer'], return: 'bool' },
    destroy_lock: { arguments: ['pointer'], return: 'void' },
    destroy_mutex: { arguments: ['pointer'], return: 'void' },
    get_last_error: { arguments: ['buffer', sizeType], return: 'bool' },
});

function lastError() {
    const buffer = Buffer.alloc(512);
    const capacity = sizeType === 'uint32' ? buffer.length : BigInt(buffer.length);
    functions.get_last_error(buffer, capacity);
    const message = buffer.toString('utf8', 0, buffer.indexOf(0));
    return new Error(message || 'lockd operation failed');
}

try {
    const mutex = functions.create_mutex('localhost', port, 5n, 'my_lock');
    if (mutex === 0n)
        throw lastError();

    try {
        const guard = functions.create_lock(mutex);
        if (guard === 0n)
            throw lastError();

        try {
            if (!functions.lock(guard))
                throw lastError();
            console.log('Lock acquired; do the protected work here.');
        }
        finally {
            functions.destroy_lock(guard);
        }
    }
    finally {
        functions.destroy_mutex(mutex);
    }
}
finally {
    lib.close();
}
```

Pass the library path, and optionally a daemon port. For example, on Windows:

```sh
node example.cjs C:/path/to/liblockd_dll.dll
```

On Linux or FreeBSD:

```sh
node example.cjs /path/to/sdk/lib/libliblockd_dll.so
```

The runtime loader must also find the library's dependencies. These calls block the calling thread,
including while connecting or waiting for a lock. In a Node.js server, run the entire operation in
one [worker thread](https://nodejs.org/api/worker_threads.html), including creation, error
retrieval, protected work, and cleanup. Keep handles private to that worker; don't dispatch
individual calls for one connection to different workers.

## API rules and errors

`create_mutex` connects immediately; its timeout is a nonnegative count of seconds within the
platform steady clock's range. `create_lock` creates an unlocked guard. `lock` waits for
acquisition, while `try_lock` returns 0 for acquired, 1 for contention, or 2 for an error.
Contention isn't an error. The C API doesn't expose the timed or recursive C++ mutex types.

Check null creation results and false operation results. Read `get_last_error` on the same thread
immediately after a failure, before cleanup. Successful calls don't clear old errors. A 512-byte
buffer is normally enough; a false result can mean truncation or no error. Capacity includes the
terminating NUL.

Only pass valid handles of the expected type. Destroy each once, locks before mutexes. Don't share a
connection between concurrent callers. `destroy_lock` releases an owned lock; cleanup isn't a
confirmation that the server received a release. See [connection
failures](USAGE.md#connection-failures).

For additional bindings, use the declarations in [the public
header](lib/liblockd_dll/include/liblockd_dll/liblockd_dll.h). Ports use `uint16_t`, timeout seconds
use `int64_t`, and `try_lock` returns `int32_t` on every supported architecture, including x86.
Buffer capacities use `size_t`; handles use pointers. Match their sizes to your application's
architecture. The library uses the C calling convention. C booleans need one-byte marshalling in the
Windows .NET examples. Strings are NUL-terminated bytes. Lock names must match
`[a-zA-Z0-9_-]{1,256}`: 1-256 ASCII letters, digits, underscores, or hyphens. Names are
case-sensitive; non-ASCII names are not supported. Invalid names make `create_mutex()` return
`NULL`; `get_last_error()` reports the validation failure.

The .NET declarations follow [Microsoft's interop
guidance](https://learn.microsoft.com/en-us/dotnet/standard/native-interop/best-practices). For Rust
ownership and FFI rules, see [the Rust FFI guide](https://doc.rust-lang.org/nomicon/ffi.html).
