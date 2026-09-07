#!/usr/bin/env python3
# ##################################################################################################
#  The MIT License (MIT)
#  Copyright (c) 2025-2026 Advanced Micro Devices, Inc. All rights reserved.
#
#  Permission is hereby granted, free of charge, to any person obtaining a copy of this software
#  and associated documentation files (the "Software"), to deal in the Software without restriction,
#  including without limitation the rights to use, copy, modify, merge, publish, distribute,
#  sublicense, and/or sell copies of the Software, and to permit persons to whom the Software is
#  furnished to do so, subject to the following conditions:
#
#  The above copyright notice and this permission notice shall be included in all copies or
#  substantial portions of the Software.
#
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT
# NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
# NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM,
# DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
# OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
# ##################################################################################################
import json
import struct
import zmq
import sys

# Kernel base addresses (must match system_map.xml)
VADD_BASE = 0x10000

# Functional arg register offsets relative to kernel base (must match system_map.xml)
VADD_IN1_OFFSET  = 0x10
VADD_IN2_OFFSET  = 0x18
VADD_OUT_OFFSET  = 0x20
VADD_SIZE_OFFSET = 0x28


def read_int32(buffer, index):
    """Read an int32 element, returning zero for missing or partial data."""
    byte_index = index * 4
    if byte_index + 4 > len(buffer):
        return 0
    return struct.unpack_from("<i", buffer, byte_index)[0]


def run_vadd(buffers, in1_key, in2_key, out_key, size):
    """Add two int32 buffers and store the result."""
    in1_bytes = buffers.get(in1_key, b"")
    in2_bytes = buffers.get(in2_key, b"")
    in1 = [read_int32(in1_bytes, index) for index in range(size)]
    in2 = [read_int32(in2_bytes, index) for index in range(size)]
    out = [a + b for a, b in zip(in1, in2)]
    buffers[out_key] = struct.pack(f"<{size}i", *out)


def reconstruct_64bit(registers, base, offset):
    """Reconstruct a 64-bit address from two consecutive 32-bit register writes."""
    lo = registers.get(base + offset, 0)
    hi = registers.get(base + offset + 4, 0)
    return (hi << 32) | lo


def main():
    if len(sys.argv) != 2:
        print(f"Usage: {sys.argv[0]} <ZMQ address>", file=sys.stderr)
        sys.exit(1)

    context = zmq.Context()
    socket = context.socket(zmq.REP)
    socket.setsockopt(zmq.LINGER, 0)
    socket.bind(sys.argv[1])

    buffers = {}
    streams = {}
    registers = {}

    while True:
        frames = [socket.recv()]
        while socket.getsockopt(zmq.RCVMORE):
            frames.append(socket.recv())

        try:
            message = json.loads(frames[0])
        except (json.JSONDecodeError, UnicodeDecodeError):
            socket.send(b"OK")
            continue

        command = message.get("command", "")

        if command == "exit":
            socket.send(b"OK")
            break

        elif command == "populate":
            key = message.get("name", str(message.get("addr", "")))
            if len(frames) > 1:
                buffers[key] = frames[1]
            socket.send(b"OK")

        elif command == "stream_in":
            key = message.get("name", "")
            if len(frames) > 1:
                streams[key] = frames[1]
            socket.send(b"OK")

        elif command == "stream_out":
            key = message.get("name", "")
            size = message.get("size", 0)
            data = streams.get(key, b"\x00" * size)
            socket.send(data)

        elif command == "fetch":
            typ = message.get("type", "")
            if typ == "buffer":
                key = message.get("name", str(message.get("addr", "")))
                if key in buffers:
                    data = list(buffers[key])
                else:
                    size = message.get("size", 0)
                    data = [0] * size
                socket.send_string(json.dumps(data))
            elif "addr" in message:
                val = registers.get(int(message["addr"]), 0)
                socket.send_string(json.dumps(val))
            elif message.get("function") == "vadd" and message.get("arg") == "size":
                val = registers.get(VADD_BASE + VADD_SIZE_OFFSET, 0)
                socket.send_string(json.dumps(val))
            else:
                address = message.get("name")
                val = registers.get(int(address), 0) if address else 0
                socket.send_string(json.dumps(val))

        elif command == "read_register":
            socket.send_string("0")

        elif command == "reg":
            address = int(message.get("addr", 0))
            val = int(message.get("val", 0))

            if val & 0x1 and address == VADD_BASE:
                # ap_start written to vadd CTRL — reconstruct args and run
                in1_addr = reconstruct_64bit(registers, VADD_BASE, VADD_IN1_OFFSET)
                in2_addr = reconstruct_64bit(registers, VADD_BASE, VADD_IN2_OFFSET)
                out_addr = reconstruct_64bit(registers, VADD_BASE, VADD_OUT_OFFSET)
                size_val = registers.get(VADD_BASE + VADD_SIZE_OFFSET, 0)
                run_vadd(buffers, str(in1_addr), str(in2_addr), str(out_addr), size_val)
                registers[address] = 0x6  # ap_done | ap_idle
            else:
                registers[address] = val
            socket.send(b"OK")

        elif command == "call":
            function = message.get("function", "")
            arguments = message.get("args", {})
            if function == "vadd":
                in1_name = arguments.get("arg0", {}).get("name", "")
                in2_name = arguments.get("arg1", {}).get("name", "")
                out_name = arguments.get("arg2", {}).get("name", "")
                size_val = arguments.get("arg3", {}).get("value", 0)
                run_vadd(buffers, in1_name, in2_name, out_name, size_val)
            socket.send(b"OK")

        elif command == "wait":
            socket.send(b"OK")

        else:
            socket.send(b"OK")

    socket.close()
    context.term()


if __name__ == "__main__":
    main()
