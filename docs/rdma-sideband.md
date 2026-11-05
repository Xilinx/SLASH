# RDMA sideband protocol, version 1

The sideband is a TCP connection that runs next to the 200G RoCE link. The
two ends of an RDMA session use it to exchange their queue pair and memory
parameters, to start tests on each other and to report results. No RDMA
payload is sent over it.

## Roles

One end is the server, which listens on a TCP port and accepts a single
connection. The other end is the client, which connects to that port. The
default port is 18515, the same one perftest uses. The role only decides who
opens the connection; once it is up, the protocol is symmetric and either end
may send any request.

In VRT, the session is set up through `vrt::Rdma` before calling `connect()`:

```cpp
auto rdma = device.getRdmaDevice();
rdma.setLocalIp("192.168.0.1");           // RoCE address of the V80
rdma.setRemoteIp("192.168.0.2");          // optional, checked against the peer's
rdma.setRole(vrt::Rdma::Role::Client);    // or Server (default)
rdma.setRemoteHost("<peer sideband ip>"); // client only
rdma.setPort(18515);                      // default
rdma.connect();
```

The end that runs the tests is called the driver. It is chosen independently
of the role, and in version 1 it is always VRT.

The sideband address is the peer's address on the regular (lab) network,
whereas the RoCE addresses belong to the 200G link. The stack uses a /24
network, so both RoCE addresses need to share the first three bytes (for
example 192.168.0.1 for the V80 and 192.168.0.2 for the peer).

## Framing

Messages are ASCII lines ending in `\n`, of at most 255 bytes each, so they
can be read with any `sscanf`-style parser. A receiver has to put back
together lines that were split across TCP segments, and split segments that
contain more than one line.

A line has the form `<VERB> [#<tag>] key=value key=value ...`, where:

- numbers are decimal or `0x` hex;
- keys may come in any order;
- unknown keys are ignored, so that later versions can add keys.

A request carries `#<tag>`, a decimal number that is unique per sender. The
answer echoes the tag, either as `OK #<tag> ...` or as
`ERR #<tag> msg=<text-without-spaces>`. `HELLO`, `QP`, `MR` and `READY` are
announcements: they have no tag and get no answer.

Some requests take a long time (for example a `RUN` of 400 MB), and the
receiver answers only when the work is done. A sender keeps at most one
request outstanding.

## Session

Before opening the TCP connection, each end brings its 200G link up and
sets its own MAC and RoCE IP address in the stack. This way, it already
answers ARP by the time the peer receives its `HELLO`.

Once the connection is ready, both ends send `HELLO`, `QP` and `MR`,
without waiting for each other:

```
HELLO v=1 name=<name> roce_ip=<dotted> qps=<n> mrs=<m>
QP i=<0..n-1> qpn=<local QPN> send_psn=<first PSN this end sends, 24 bit> rkey=0 udp=4791
MR i=<0..m-1> addr=<base address the peer puts in WRITE/READ> len=<bytes>
```

The queue pairs are paired by their index `i`, so queue pair `i` of one end
talks to queue pair `i` of the other. Each end then configures its stack as
follows:

- local QPN: its own `qpn`;
- remote QPN: the peer's `qpn`;
- first PSN sent: its own `send_psn`;
- first PSN expected: the peer's `send_psn`.

The PSNs should be new random 24-bit values in every session, so that a
stale packet from an earlier run is detected instead of being accepted.

After that, each end resolves the peer's `roce_ip` through ARP, connects its
queue pairs and sends `READY`. No RDMA operation is posted before both ends
have sent and received `READY`. If the versions (`v`) differ, or the link or
ARP fails, the end sends `ERR msg=...` (without a tag) and closes the
connection.

## Requests

The requests are sent by the driver, although in principle either end may
send them.

| Request | What the receiver does | Answer |
|---|---|---|
| `FILL #t mr= off= len= seed=` | writes the pattern into its own MR `mr`, `len` bytes starting at byte `off` | `OK #t` |
| `CHECK #t mr= off= len= seed=` | compares that range of its own MR with the pattern | `OK #t errors=<wrong words> first=<byte offset of the first wrong word, or -1>` |
| `RUN #t op=write\|read qp= count= size= laddr= raddr=` | posts `count` RDMA operations of `size` bytes on its queue pair with index `qp` (the `i` of `QP`, not a QPN), operation `k` going from/to local `laddr + k*size` and remote `raddr + k*size`, then waits for all of them to complete | `OK #t ns=<elapsed> retrans=<delta> psn_drops=<delta>` |
| `STATS #t` | reports its counters | `OK #t key=value ...` (a free list: received and sent frames, retransmissions, PSN drops, receive drops, ...) |
| `BYE` | closes the connection after answering | `OK` (without a tag) |

For `RUN`, `laddr` is an address in the receiver's own address space,
whereas `raddr` is the address the receiver puts on the wire, inside one of
the requester's MRs. The `addr` of an MR is also a valid `laddr` for the end
that announced it, so the requester can name the receiver's memory by the
addresses the receiver itself announced.

On the V80, a local address can also be the stream address (bit 47 set),
which stands for the user-region stream: `rdma.tx` for what is sent and
`rdma.rx` for what is received. The peer does not need to know about this.

In version 1, VRT is always the driver. It answers a `STATS` or a `BYE`
from the peer whenever it reads the sideband (that is, while waiting for
its own requests), and it answers `FILL`, `CHECK` and `RUN` with
`ERR #t msg=unsupported`. The peer therefore has to serve all requests.

## Pattern

For a range with seed `s`, the 32-bit little-endian word at byte offset
`4*k` from the start of the range is `(s + k) mod 2^32`. The ranges of
`FILL` and `CHECK`, and the `count*size` span of a `RUN`, are contiguous.
This means that a misplaced, repeated or missing message shows up as wrong
words. `len` and `size` are multiples of 4.

A peer that checks the data as it arrives (in a separate MR for incoming
data) can use an address pattern instead: the 64-bit word at peer address
`a` is `(s << 47) | a[46:0]`. The example below picks this pattern when the
peer announces separate MRs for incoming and outgoing data, and the plain
pattern otherwise (`--pattern` overrides the choice).

## Tests of the end-to-end example

The examples `08_rdma` (card memory) and `09_rdma_stream` (user-region
stream) run four tests for each message size, similar to `perftest -a`.
Each transfer is checked by the end that receives the data:

1. V80 WRITE: VRT fills its memory, posts `count` WRITEs into the peer's
   MR, waits for them to complete and sends `CHECK` to the peer.
2. Peer READ: VRT fills its memory and sends `RUN op=read` to the peer,
   then `CHECK`.
3. Peer WRITE: VRT sends `FILL` and then `RUN op=write` to the peer, aimed
   at its own memory, and checks what arrived.
4. V80 READ: VRT sends `FILL` to the peer, posts `count` READs into its
   memory and checks them.

When the peer has a separate MR for incoming data, VRT also sends it a
`FILL` with the seed before tests 1 and 2, so that the peer knows what to
expect (in plain memory, such a `FILL` would hide lost WRITEs, so it is
not sent). At the end, VRT prints its own counters and the peer's `STATS`,
and sends `BYE`. The
example prints one line per message size, with the rate of every test,
the retransmissions and the check errors. It exits with 0 only if every
check has 0 errors.

By default, the message size goes from 128B to 1MB, doubling, with 2000
messages per size and test (`--size`, `--min`, `--max` and `--count`
change this). It needs to be noted that the V80 posts each of its own
messages with one register write from the host, which limits the rate of
the V80 WRITE and READ tests for small messages. The tests posted by the
peer are not affected by this, and they show the rate of the link itself.
