"""A small client for t-code's plugin socket (tapi_1), standard library only.

The same file sits in each example plugin's folder: a registry entry pins one folder, so each
plugin carries what it needs. See t-code's docs/plugins.md for the protocol.

    t = Tcode()                    # connects to $TCODE_SOCKET, says hello with the plugin's token
    t.request("say", "hi")         # "ok" -> "", "ok <value>" -> "<value>", "error ..." -> TcodeError
    t.text("get-selection")        # the requests answering "ok", then text and NUL
    for event in Tcode().events(): # a second connection, subscribed: one dict per event
        ...
"""
import json
import os
import socket


class TcodeError(Exception):
    def __init__(self, code, message):
        super().__init__(f"{code}: {message}")
        self.code = code
        self.message = message


class Tcode:
    def __init__(self, path=None, token=None):
        self.sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.sock.connect(path or os.environ["TCODE_SOCKET"])
        self.file = self.sock.makefile("rb")
        token = os.environ.get("TCODE_PLUGIN_TOKEN", "") if token is None else token
        self.request("hello", f"tapi_1 {token}".strip())

    def close(self):
        self.file.close()
        self.sock.close()

    def send(self, command, argument=""):
        # Every request ends with NUL from the first one on: an argument may hold line breaks.
        self.sock.sendall(f"{command} {argument}".encode() + b"\0")

    def answer(self):
        line = self.file.readline()
        if not line:
            raise TcodeError("closed", "t-code closed the connection")
        line = line.decode().rstrip("\n")
        if line == "ok" or line.startswith("ok "):
            return line[3:]
        code, _, message = line.partition(" ")[2].partition(" ")
        raise TcodeError(code or "unknown", message)

    def request(self, command, argument=""):
        self.send(command, argument)
        return self.answer()

    def json(self, command, value):
        return self.request(command, json.dumps(value, separators=(",", ":"), ensure_ascii=False))

    def text(self, command):
        """get-text, get-selection: "ok", then the text up to a NUL byte."""
        self.request(command)
        data = bytearray()
        while True:
            byte = self.file.read(1)
            if not byte or byte == b"\0":
                return data.decode()
            data += byte

    def events(self):
        """Subscribes this connection: yields each event (a dict) until t-code goes away."""
        self.request("subscribe")
        for line in self.file:
            try:
                yield json.loads(line)
            except ValueError:
                continue
