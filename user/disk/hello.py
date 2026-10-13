# python on serika: the standard library, big numbers, files, threads, sockets, a child process. each failure is a number.
# the exit code of the end is 0x5e, as the other test programs have

import sys


def check(ok, n):
    if not ok:
        print("python: check", n, "failed")
        sys.exit(n)


check(sys.version_info >= (3, 10), 1)

import hashlib, json, math, re, struct, zlib, collections, itertools, functools

check(json.loads(json.dumps({"a": [1, 2.5, None], "b": "é"})) == {"a": [1, 2.5, None], "b": "é"}, 2)
check(math.isclose(math.sqrt(2) ** 2, 2), 3)
check(hashlib.sha256(b"serika").hexdigest() == "8c6044d47872b57c6144e7fe8abaeecb66f6036a6fd805ec1d4229a9614b64ed", 4)
check(zlib.decompress(zlib.compress(b"x" * 10000)) == b"x" * 10000, 5)
check(struct.unpack("<I", struct.pack("<I", 0xdeadbeef))[0] == 0xdeadbeef, 6)
check(sum(i * i for i in range(100000)) == 333328333350000, 7)
check(2 ** 200 // 3 ** 50 == 2238393297946874000179418290327143433, 8)
check(re.findall(r"\d+", "a1b22c333") == ["1", "22", "333"], 9)
check(sorted(collections.Counter("banana").items())[0] == ("a", 3), 10)
check(functools.reduce(lambda a, b: a * b, range(1, 21)) == 2432902008176640000, 11)

# files: a directory of its own, a file written, read back, renamed, listed, and gone
import os, tempfile

with tempfile.TemporaryDirectory(dir="/tmp") as d:
    path = os.path.join(d, "a.txt")

    with open(path, "w", encoding="utf-8") as f:
        f.write("한글 and ascii\n" * 1000)

    check(os.stat(path).st_size == len("한글 and ascii\n".encode()) * 1000, 12)

    os.rename(path, path + ".b")
    check(os.listdir(d) == ["a.txt.b"], 13)

    with open(path + ".b", encoding="utf-8") as f:
        check(f.read().count("한글") == 1000, 14)

check(not os.path.exists(d), 15)

# threads that share a counter
import threading

count = 0
lock = threading.Lock()


def work():
    global count

    for _ in range(2000):
        with lock:
            count += 1


threads = [threading.Thread(target=work) for _ in range(4)]

for t in threads:
    t.start()

for t in threads:
    t.join()

check(count == 8000, 16)

# a tcp server and a client on lo: a megabyte there and back
import socket

server = socket.socket()
server.bind(("127.0.0.1", 0))
server.listen(1)
port = server.getsockname()[1]
data = bytes(range(256)) * 4096


def echo():
    conn, _ = server.accept()

    with conn:
        while True:
            chunk = conn.recv(65536)

            if not chunk:
                break

            conn.sendall(chunk)

        conn.shutdown(socket.SHUT_WR)


t = threading.Thread(target=echo)
t.start()

with socket.create_connection(("127.0.0.1", port)) as c:
    # the sending is a thread of its own: a megabyte is more than the buffers of both ends hold, and if the same thread did not
    # read while the echo is sent back, both would wait for the other for ever
    def send():
        c.sendall(data)
        c.shutdown(socket.SHUT_WR)

    sender = threading.Thread(target=send)
    sender.start()

    got = bytearray()

    while True:
        chunk = c.recv(65536)

        if not chunk:
            break

        got += chunk

sender.join()
t.join()
server.close()
check(bytes(got) == data, 17)

# time
import time

t0 = time.monotonic()
time.sleep(0.1)
check(0.09 < time.monotonic() - t0 < 2, 18)

# a child process, its output through a pipe, and its exit code
import subprocess

r = subprocess.run(["/bin/echo", "from a child"], capture_output=True, text=True)
check(r.returncode == 0 and r.stdout == "from a child\n", 19)
check(subprocess.run(["/bin/false"]).returncode == 1, 20)

print("python", sys.version.split()[0], "ok")
sys.exit(0x5e)
