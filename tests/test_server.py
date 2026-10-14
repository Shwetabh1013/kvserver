"""Integration tests: start the real server binary and talk to it over TCP."""
import os, signal, socket, subprocess, sys, time

BIN = os.environ.get("KV_BIN", "./build/kvserver")
PORT = int(os.environ.get("KV_PORT", "7071"))


class Client:
    def __init__(self):
        self.s = socket.create_connection(("127.0.0.1", PORT), timeout=5)
        self.f = self.s.makefile("rb")

    def cmd(self, line):
        self.s.sendall(line.encode() + b"\n")
        return self.f.readline().decode().rstrip("\n")

    def close(self):
        self.s.close()


def check(name, got, want):
    if got != want:
        print(f"FAIL {name}: got {got!r}, want {want!r}")
        sys.exit(1)
    print(f"PASS {name}")


def main():
    proc = subprocess.Popen([BIN, "-p", str(PORT), "-t", "4"], stdout=subprocess.PIPE)
    try:
        for _ in range(50):
            try:
                socket.create_connection(("127.0.0.1", PORT), timeout=1).close()
                break
            except OSError:
                time.sleep(0.1)

        c = Client()
        check("PING", c.cmd("PING"), "+PONG")
        check("GET missing", c.cmd("GET nope"), "$-1")
        check("SET", c.cmd("SET name Shwetabh"), "+OK")
        check("GET hit", c.cmd("GET name"), "$8 Shwetabh")
        check("value with spaces", (c.cmd("SET msg hello world"), c.cmd("GET msg"))[1], "$11 hello world")
        check("overwrite", (c.cmd("SET name kv"), c.cmd("GET name"))[1], "$2 kv")
        check("COUNT", c.cmd("COUNT"), ":2")
        check("DEL present", c.cmd("DEL name"), ":1")
        check("DEL absent", c.cmd("DEL name"), ":0")
        check("bad SET", c.cmd("SET onlykey"), "-ERR usage: SET <key> <value>")
        check("unknown command", c.cmd("FLY"), "-ERR unknown command")
        check("CRLF tolerated", c.cmd("PING\r"), "+PONG")

        # pipelining: several commands in a single send()
        c.s.sendall(b"SET p 1\nGET p\nPING\n")
        got = [c.f.readline().decode().rstrip("\n") for _ in range(3)]
        check("pipelined commands", got, ["+OK", "$1 1", "+PONG"])

        # a command split across two TCP segments
        c.s.sendall(b"GET ")
        time.sleep(0.05)
        c.s.sendall(b"p\n")
        check("partial read reassembly", c.f.readline().decode().rstrip("\n"), "$1 1")

        # second client sees data written by the first (shared store)
        d = Client()
        check("shared across connections", d.cmd("GET msg"), "$11 hello world")
        d.close()

        check("QUIT", c.cmd("QUIT"), "+BYE")
        c.close()

        # graceful shutdown on SIGINT
        proc.send_signal(signal.SIGINT)
        out, _ = proc.communicate(timeout=5)
        check("graceful SIGINT shutdown", (proc.returncode, b"clean exit" in out), (0, True))
        print("all server tests passed")
    finally:
        if proc.poll() is None:
            proc.kill()


if __name__ == "__main__":
    main()
