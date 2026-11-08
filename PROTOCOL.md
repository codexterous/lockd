# Daemon protocol

lockd uses a line-oriented text protocol over TCP, normally on port 7198. Connections own locks. Use
the client libraries unless you're implementing a new client or inspecting the daemon by hand.

**Do not share a protocol connection between threads.** The protocol itself is not thread-safe. Each
caller needs its own connection. Commands and replies on one connection form a single conversation;
the protocol does not separate concurrent callers or identify which caller owns a reply. Separate
connections can contend for the same lock name.

## Commands and replies

Commands are case-insensitive; lock names retain their spelling and are case-sensitive. End commands
with LF or CRLF. Replies use CRLF. Each connection can hold or wait for one lock at a time, and must
release it before requesting another.

| Command                 | Result                                                                          |
|-------------------------|---------------------------------------------------------------------------------|
| `LOCK name`             | `LOCK_ACQUIRING`, then `LOCK_ACQUIRED` when ownership is granted                |
| `TRY_LOCK name`         | `LOCK_ACQUIRED` or `LOCK_UNAVAILABLE`                                           |
| `TIMED_LOCK name 500ms` | `LOCK_ACQUIRING`, then `LOCK_ACQUIRED` or `LOCK_TIMED_OUT`                      |
| `UNLOCK`                | `LOCK_RELEASED` after releasing an owned lock                                   |
| `INFO`                  | Three lines describing this client, its lock status, and connected client count |
| `VERSION`               | Daemon and client-library version text                                          |
| `QUIT`                  | Close the connection                                                            |

Timed waits accept an unsigned integer followed by `ms`, `s`, `m`, or `h`; omitting the suffix means
seconds. Durations must fit the daemon's steady clock; excessive waits receive `INVALID_ARGS`. Lock
names must match `[a-zA-Z0-9_-]{1,256}`: 1-256 ASCII letters, digits, underscores, or hyphens. Names
are case-sensitive; characters outside that alphabet receive `INVALID_COMMAND`. Oversized names
using the permitted characters receive `LOCK_NAME_TOO_LONG` if the command fits within the wire
limit below. Replies such as `INVALID_COMMAND`, `INVALID_ARGS`, `MISSING_LOCK_NAME`, `UNLOCK_FIRST`,
and `LOCK_FIRST` describe invalid requests.

For example, an uncontended exchange is:

```text
> LOCK example
< LOCK_ACQUIRING
< LOCK_ACQUIRED
> UNLOCK
< LOCK_RELEASED
```

Read complete lines and account for the intermediate `LOCK_ACQUIRING` reply. TCP reads need not
correspond to commands or replies: a line can arrive in pieces, or several replies can arrive
together.

The first `INFO` line is `you are client 0x<identifier> at <ip>`. The opaque identifier has 11
lowercase hexadecimal digits prefixed with `0x`. It remains the same for that connection in INFO and
daemon logs. Reconnecting generates another identifier. Identifiers are diagnostic labels; they are
not guaranteed to be unique and are not authentication credentials.

## Connection lifetime

Each command is limited to 1,024 wire bytes, including its terminator, whitespace, and Backspace or
DEL bytes before editing. This limit applies separately to each command in a batch. If the input
buffer fills without a terminator, the daemon closes the connection without a reply and discards the
incomplete command.

Once the daemon receives the first byte of a command, it allows 30 seconds to receive the complete
command. Sending more bytes does not extend this deadline. Expired deadlines are checked every five
seconds, so an incomplete command normally closes the connection after 30-35 seconds, subject to
worker scheduling, without a reply. Idle connections and completed requests waiting for or holding a
lock have no command read deadline. Applications may leave a connection open before its first
request and between lock operations while doing other, unprotected work.

The daemon bounds queued replies and pauses command reads when a client falls behind. Exceeding the
reply limit closes the connection. Clients should read replies as they send commands rather than
send unbounded batches.

While replies are pending, the socket must make write progress within 30 seconds. Each successful
partial write restarts this deadline; adding replies does not. If writing stalls, the daemon closes
the connection on a periodic check, normally after 30-35 seconds without progress, subject to worker
scheduling. A connection with no replies pending has no write deadline.

Disconnecting releases the connection's held lock or cancels its queued request once the daemon
observes the disconnect. There are no durable ownership records, leases, authentication, or fencing
tokens. A graceful stop drains current owners for a bounded interval; see
[RUNNING.md](RUNNING.md#graceful-shutdown) and [USAGE.md](USAGE.md#connection-failures).

## Manual connections

The lockd protocol is a simple text-based one. You can connect to the server with a telnet client
using local echo and line editing.

Commands end with LF or CRLF. Before parsing a command, the daemon treats Backspace (`0x08`) and DEL
(`0x7f`) as removal of the previous byte on that line. Either at the start of a line does nothing,
and edits never reach a previously submitted command. The daemon leaves terminal echo to the client
and does not negotiate Telnet options. Ctrl-D and Ctrl-Z close the connection without requiring a
newline.
