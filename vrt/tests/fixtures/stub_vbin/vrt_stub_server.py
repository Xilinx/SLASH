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
import zmq

def main():
    ctx = zmq.Context()
    sock = ctx.socket(zmq.REP)
    sock.bind("tcp://*:5555")

    buffers = {}
    streams = {}
    registers = {}

    while True:
        frames = [sock.recv()]
        while sock.getsockopt(zmq.RCVMORE):
            frames.append(sock.recv())

        try:
            msg = json.loads(frames[0])
        except (json.JSONDecodeError, UnicodeDecodeError):
            sock.send(b"OK")
            continue

        cmd = msg.get("command", "")

        if cmd == "exit":
            sock.send(b"OK")
            break

        elif cmd == "populate":
            key = msg.get("name", str(msg.get("addr", "")))
            if len(frames) > 1:
                buffers[key] = frames[1]
            sock.send(b"OK")

        elif cmd == "stream_in":
            key = msg.get("name", "")
            if len(frames) > 1:
                streams[key] = frames[1]
            sock.send(b"OK")

        elif cmd == "stream_out":
            key = msg.get("name", "")
            size = msg.get("size", 0)
            data = streams.get(key, b"\x00" * size)
            sock.send(data)

        elif cmd == "fetch":
            typ = msg.get("type", "")
            if typ == "buffer":
                key = msg.get("name", str(msg.get("addr", "")))
                if key in buffers:
                    data = list(buffers[key])
                else:
                    size = msg.get("size", 0)
                    data = [0] * size
                sock.send_string(json.dumps(data))
            else:
                addr = str(msg.get("addr", msg.get("name", "")))
                val = registers.get(addr, 0)
                sock.send_string(json.dumps(val))

        elif cmd == "read_register":
            sock.send_string("0")

        elif cmd == "reg":
            addr = str(msg.get("addr", ""))
            val = int(msg.get("val", 0))
            if val & 0x1:
                registers[addr] = 0x6
            else:
                registers[addr] = val
            sock.send(b"OK")

        elif cmd == "wait":
            sock.send(b"OK")

        else:
            sock.send(b"OK")

    sock.close()
    ctx.term()

if __name__ == "__main__":
    main()
