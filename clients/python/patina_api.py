#!/usr/bin/env python3
"""Python client for the Patina HTTP API (standard library only, Python 3.8+).

Start a server:   patina --headless --http                    (this machine)
                  patina --headless --lan --workspace /srv/pt  (local network, token)

    from patina_api import Patina
    pt = Patina("http://render-07:7870", token="...")
    pt.upload("crate_low.glb", "in/crate_low.glb")
    pt.upload("crate_high.glb", "in/crate_high.glb")
    job = pt.run([
        ("open_file", {"path": "in/crate_low.glb"}),
        ("set_resolution", {"resolution": 4096}),
        ("bake_from_high_poly", {"path": "in/crate_high.glb"}),
        ("export_textures", {"directory": "out/crate", "preset": "unreal"}),
    ], name="crate", on_progress=print)
    for f in pt.files("out/crate"):
        pt.download(f["path"], "textures/" + f["path"].split("/")[-1])

Relative paths in command arguments are inside the server's workspace; upload() and
download() move files in and out of it.
"""
import json
import os
import shutil
import time
import urllib.error
import urllib.parse
import urllib.request

__all__ = ["Patina", "PatinaError", "JobFailed"]


class PatinaError(RuntimeError):
    """A request failed. `status` is the HTTP status, `body` the decoded JSON reply."""

    def __init__(self, message, status=None, body=None):
        super().__init__(message)
        self.status = status
        self.body = body


class JobFailed(PatinaError):
    """A job ended as failed or cancelled. `job` is its final status."""

    def __init__(self, job):
        super().__init__("job %s %s: %s" % (job.get("id"), job.get("status"), job.get("error", "")), body=job)
        self.job = job


class Patina:
    def __init__(self, url="http://127.0.0.1:7870", token=None, timeout=600):
        self.url = url.rstrip("/")
        if not self.url.endswith("/api/v1"):
            self.url += "/api/v1"
        self.token = token if token is not None else os.environ.get("PATINA_TOKEN")
        self.timeout = timeout

    # ---- plumbing
    def _request(self, method, path, body=None, raw=None, params=None, timeout=None, stream_to=None):
        url = self.url + path
        if params:
            url += "?" + urllib.parse.urlencode(params)
        headers = {}
        data = None
        if self.token:
            headers["Authorization"] = "Bearer " + self.token
        if raw is not None:
            data = raw
            headers["Content-Type"] = "application/octet-stream"
        elif body is not None:
            data = json.dumps(body).encode()
            headers["Content-Type"] = "application/json"
        req = urllib.request.Request(url, data=data, method=method, headers=headers)
        try:
            with urllib.request.urlopen(req, timeout=timeout or self.timeout) as r:
                if stream_to is not None:
                    shutil.copyfileobj(r, stream_to, 1 << 20)
                    return None
                payload = r.read()
                ctype = r.headers.get("Content-Type", "")
                return json.loads(payload) if "json" in ctype else payload
        except urllib.error.HTTPError as e:
            text = e.read()
            try:
                j = json.loads(text)
            except Exception:
                j = {"error": text.decode(errors="replace")}
            raise PatinaError("%s %s: %s %s" % (method, path, e.code, j.get("error", "")), e.code, j) from None
        except urllib.error.URLError as e:
            raise PatinaError("cannot reach Patina at %s: %s" % (self.url, e.reason)) from None

    # ---- server
    def health(self):
        return self._request("GET", "/health")

    def info(self):
        return self._request("GET", "/info")

    def commands(self):
        return self._request("GET", "/commands")["result"]

    def openapi(self):
        return self._request("GET", "/openapi.json")

    # ---- commands (synchronous)
    def call(self, cmd, args=None, timeout=None, busy_wait=30, **kwargs):
        """Runs one command now and returns its result. Keyword arguments are merged
        into `args`, so pt.call("set_brush", size=40) works."""
        a = dict(args or {})
        a.update(kwargs)
        r = self._request("POST", "/commands/" + cmd, body=a,
                          params={"busy_wait": busy_wait, **({"timeout": int(timeout)} if timeout else {})},
                          timeout=(timeout or self.timeout) + busy_wait + 10)
        return r.get("result")

    # ---- jobs (asynchronous)
    @staticmethod
    def _steps(steps):
        out = []
        for s in steps:
            if isinstance(s, dict):
                out.append(s)
            else:
                cmd, args = (s[0], s[1] if len(s) > 1 else {})
                out.append({"cmd": cmd, "args": args})
        return out

    def submit(self, steps, name=None, stop_on_error=True):
        """Queues a job; `steps` is a list of (cmd, args) pairs or {"cmd", "args"} dicts."""
        r = self._request("POST", "/jobs", body={"name": name or "", "stop_on_error": stop_on_error,
                                                 "steps": self._steps(steps)})
        return r["id"]

    def job(self, job_id, wait=0):
        return self._request("GET", "/jobs/" + job_id, params={"wait": wait} if wait else None,
                             timeout=self.timeout + wait)

    def jobs(self):
        return self._request("GET", "/jobs")["jobs"]

    def cancel(self, job_id):
        return self._request("DELETE", "/jobs/" + job_id)

    def wait(self, job_id, timeout=None, on_progress=None, poll=2.0, raise_on_failure=True):
        """Waits for a job. on_progress(job) is called while it runs."""
        t0 = time.time()
        while True:
            j = self.job(job_id, wait=poll if on_progress else min(30, timeout or 30))
            if j["status"] not in ("queued", "running"):
                if raise_on_failure and j["status"] != "done":
                    raise JobFailed(j)
                return j
            if on_progress:
                on_progress(j)
            if timeout and time.time() - t0 > timeout:
                raise PatinaError("job %s still %s after %.0f s" % (job_id, j["status"], timeout), body=j)

    def run(self, steps, name=None, stop_on_error=True, timeout=None, on_progress=None):
        """submit() + wait(): returns the finished job (results per step in job["results"])."""
        return self.wait(self.submit(steps, name, stop_on_error), timeout=timeout, on_progress=on_progress)

    # ---- files in the workspace
    def upload(self, local_path, remote_path=None):
        remote_path = remote_path or os.path.basename(local_path)
        with open(local_path, "rb") as f:
            data = f.read()
        r = self._request("PUT", "/files/" + urllib.parse.quote(remote_path), raw=data)
        return r["relative"]

    def download(self, remote_path, local_path=None):
        local_path = local_path or os.path.basename(remote_path)
        d = os.path.dirname(local_path)
        if d:
            os.makedirs(d, exist_ok=True)
        tmp = local_path + ".part"
        with open(tmp, "wb") as f:
            self._request("GET", "/files/" + urllib.parse.quote(remote_path), stream_to=f)
        os.replace(tmp, local_path)
        return local_path

    def files(self, directory="", recursive=False):
        params = {"dir": directory}
        if recursive:
            params["recursive"] = 1
        return self._request("GET", "/files", params=params)["files"]

    def delete(self, remote_path):
        return self._request("DELETE", "/files/" + urllib.parse.quote(remote_path))


if __name__ == "__main__":
    import argparse

    ap = argparse.ArgumentParser(description="Talk to a Patina HTTP API")
    ap.add_argument("--url", default=os.environ.get("PATINA_URL", "http://127.0.0.1:7870"))
    ap.add_argument("--token", default=None)
    ap.add_argument("command", help="a Patina command, or 'health' / 'jobs'")
    ap.add_argument("args", nargs="?", default="{}", help="JSON arguments")
    a = ap.parse_args()
    pt = Patina(a.url, a.token)
    if a.command == "health":
        print(json.dumps(pt.health(), indent=2))
    elif a.command == "jobs":
        print(json.dumps(pt.jobs(), indent=2))
    else:
        print(json.dumps(pt.call(a.command, json.loads(a.args)), indent=2))
