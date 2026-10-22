"""Shared plumbing for the Patina Assistant data tools: paths, a client for Patina's JSON-lines
command server, and a pool of headless Patina servers (each with its own home folder, stopped by
PID). Standard library only."""
import json
import os
import shutil
import socket
import subprocess
import sys
import tempfile
import time

REPO = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
ML = os.environ.get("PATINA_ML", os.path.expanduser("~/patina-ml"))
DATA = os.path.join(ML, "data")
SCENES = os.path.join(ML, "scenes")
REPORTS = os.path.join(ML, "reports")
LOGS = os.path.join(ML, "logs")


def patina_bin():
    b = os.environ.get("PATINA_BIN") or os.path.join(REPO, "build", "patina")
    if not os.path.exists(b):
        sys.exit("Patina binary not found: %s (build it, or set PATINA_BIN)" % b)
    return b


def ensure_dirs():
    for d in (DATA, SCENES, REPORTS, LOGS):
        os.makedirs(d, exist_ok=True)


class CommandFailed(RuntimeError):
    def __init__(self, cmd, message):
        super().__init__("%s: %s" % (cmd, message))
        self.cmd, self.message = cmd, message


class Client:
    """One connection to a Patina command server."""

    def __init__(self, port, host="127.0.0.1", timeout=600):
        self.sock = socket.create_connection((host, port), timeout=timeout)
        self.f = self.sock.makefile("rwb")
        self.n = 0

    def __call__(self, cmd, **args):
        return self.call(cmd, args)

    def call(self, cmd, args=None):
        self.n += 1
        self.f.write((json.dumps({"id": self.n, "cmd": cmd, "args": args or {}}) + "\n").encode())
        self.f.flush()
        line = self.f.readline()
        if not line:
            raise CommandFailed(cmd, "the server closed the connection")
        r = json.loads(line)
        if "error" in r or not r.get("ok", True):
            raise CommandFailed(cmd, r.get("error", "failed"))
        return r.get("result")

    def close(self):
        try:
            self.sock.close()
        except OSError:
            pass


def free_port():
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    port = s.getsockname()[1]
    s.close()
    return port


def _die_with_parent():
    """Linux: the server gets SIGTERM when the process that started it ends, however it ends
    (a pool worker that is terminated, a crash), so no headless Patina is ever left running."""
    try:
        import ctypes
        import signal
        ctypes.CDLL("libc.so.6", use_errno=True).prctl(1, signal.SIGTERM)  # PR_SET_PDEATHSIG
    except Exception:  # noqa: BLE001 - other systems: Server.stop() still runs normally
        pass


class Server:
    """A headless Patina on its own port and home folder."""

    def __init__(self, resolution=512, log_dir=None):
        self.port = free_port()
        self.home = tempfile.mkdtemp(prefix="patina-ml-home-")
        env = dict(os.environ, HOME=self.home, XDG_CONFIG_HOME=os.path.join(self.home, ".config"),
                   XDG_DATA_HOME=os.path.join(self.home, ".local", "share"), PATINA_PORT=str(self.port))
        log = open(os.path.join(log_dir or LOGS, "server-%d.log" % self.port), "w")
        self.proc = subprocess.Popen([patina_bin(), "--headless", "--port", str(self.port), "--resolution",
                                      str(resolution)], env=env, stdout=log, stderr=subprocess.STDOUT,
                                     preexec_fn=_die_with_parent)
        self.client = None
        for _ in range(300):
            if self.proc.poll() is not None:
                raise RuntimeError("Patina exited at start (see %s)" % log.name)
            try:
                self.client = Client(self.port)
                break
            except OSError:
                time.sleep(0.05)
        if not self.client:
            self.stop()
            raise RuntimeError("Patina did not start")

    def __call__(self, cmd, **args):
        return self.client.call(cmd, args)

    def call(self, cmd, args=None):
        return self.client.call(cmd, args)

    def stop(self):
        if self.client:
            self.client.close()
        if self.proc.poll() is None:
            self.proc.terminate()  # by PID, never by name
            try:
                self.proc.wait(timeout=10)
            except subprocess.TimeoutExpired:
                self.proc.kill()
                self.proc.wait()
        shutil.rmtree(self.home, ignore_errors=True)

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.stop()


def jsonl_read(path):
    with open(path) as f:
        for line in f:
            line = line.strip()
            if line:
                yield json.loads(line)


def jsonl_write(path, rows):
    tmp = path + ".tmp"
    n = 0
    with open(tmp, "w") as f:
        for r in rows:
            f.write(json.dumps(r, ensure_ascii=False) + "\n")
            n += 1
    os.replace(tmp, path)
    return n
