#!/usr/bin/env python3
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
