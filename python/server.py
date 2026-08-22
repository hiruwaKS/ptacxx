#!/usr/bin/env python3
"""Single-threaded HTTP wrapper around a persistent query-response program.

Usage:
    python3 server.py --cmd irm lib.bc --note-folder notes
    python3 server.py --port-start 9000 --port-end 9010 --cmd python3 agent.py prompt.txt

The program is started once at launch. For each query, server.py writes the
query line to the program's stdin and reads stdout until the program emits:
    <queryresult>...</queryresult>

The printed URL accepts POST /query with JSON {"query": "one line"}.
HTTPServer is deliberately single-threaded, so requests are not concurrent.
"""

import argparse
import json
import queue
import subprocess
import sys
import threading
from http.server import BaseHTTPRequestHandler, HTTPServer

RED = "\033[31m"
RESET = "\033[0m"
HOST = "127.0.0.1"


class QueryResponseProcess:
    RESULT_START = "<queryresult>"
    RESULT_END = "</queryresult>"

    def __init__(self, command):
        self.command = command
        self.proc = subprocess.Popen(
            command,
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            encoding="utf-8",
            errors="replace",
            bufsize=1,
        )
        self.lock = threading.Lock()
        self.stdout_queue = queue.Queue()
        self.non_result_queue = queue.Queue()
        self.stdout_thread = threading.Thread(target=self._drain_stdout, daemon=True)
        self.stderr_thread = threading.Thread(target=self._drain_stderr, daemon=True)
        self.startup_timer = None
        self.has_queried = False
        self.stdout_thread.start()
        self.stderr_thread.start()
        self._start_startup_timer()

    def _print_red(self, text):
        sys.stderr.write(f"{RED}{text}{RESET}")
        sys.stderr.flush()

    def _queue_red(self, text):
        if self.has_queried:
            self._print_red(text)
        else:
            self.non_result_queue.put(text)

    def _start_startup_timer(self):
        self.startup_timer = threading.Timer(0.2, self._startup_timer_loop)
        self.startup_timer.daemon = True
        self.startup_timer.start()

    def _startup_timer_loop(self):
        with self.lock:
            if self.has_queried:
                return
            while True:
                try:
                    text = self.non_result_queue.get_nowait()
                except queue.Empty:
                    break
                self._print_red(text)
            self._start_startup_timer()

    def _drain_stdout(self):
        in_result = False
        for line in self.proc.stdout:
            self.stdout_queue.put(line)
            if in_result:
                end = line.find(self.RESULT_END)
                if end != -1:
                    in_result = False
                    suffix = line[end + len(self.RESULT_END):]
                    if suffix:
                        self._queue_red(suffix)
                continue

            marker = line.find(self.RESULT_START)
            if marker == -1:
                self._queue_red(line)
                continue

            prefix = line[:marker]
            if prefix:
                self._queue_red(prefix)

            in_result = True
            rest = line[marker + len(self.RESULT_START):]
            end = rest.find(self.RESULT_END)
            if end != -1:
                in_result = False
                suffix = rest[end + len(self.RESULT_END):]
                if suffix:
                    self._queue_red(suffix)
        self.stdout_queue.put(None)

    def _drain_stderr(self):
        for line in self.proc.stderr:
            self._queue_red(line)

    def query(self, text):
        with self.lock:
            self.has_queried = True
            if self.startup_timer is not None:
                self.startup_timer.cancel()
                self.startup_timer = None
            while True:
                try:
                    text = self.non_result_queue.get_nowait()
                except queue.Empty:
                    break
                self._print_red(text)
            if self.proc.poll() is not None:
                raise RuntimeError(f"program exited with code {self.proc.returncode}")
            try:
                self.proc.stdin.write(text + "\n")
                self.proc.stdin.flush()
            except (BrokenPipeError, OSError) as exc:
                raise RuntimeError(f"failed to write to program: {exc}") from exc

            payload = []
            in_result = False
            while True:
                line = self.stdout_queue.get()
                if line is None:
                    raise RuntimeError("program exited while waiting for query result")
                if not in_result:
                    marker = line.find(self.RESULT_START)
                    if marker == -1:
                        continue
                    line = line[marker + len(self.RESULT_START):]
                    in_result = True
                    if not line.strip():
                        continue

                end = line.find(self.RESULT_END)
                if end != -1:
                    payload.append(line[:end])
                    return "".join(payload)
                payload.append(line)

    def close(self):
        try:
            if self.proc.poll() is None:
                self.proc.terminate()
                self.proc.wait(timeout=5)
        except Exception:
            self.proc.kill()


def make_handler(process):
    class Handler(BaseHTTPRequestHandler):
        def _send_json(self, status, payload):
            body = json.dumps(payload, ensure_ascii=False).encode("utf-8")
            self.send_response(status)
            self.send_header("Content-Type", "application/json; charset=utf-8")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)

        def do_GET(self):
            if self.path.split("?", 1)[0] == "/health":
                self._send_json(200, {"ok": True})
            else:
                self._send_json(404, {"ok": False, "error": "not found"})

        def do_POST(self):
            if self.path.split("?", 1)[0] != "/query":
                self._send_json(404, {"ok": False, "error": "not found"})
                return

            try:
                length = int(self.headers.get("Content-Length", "0"))
            except ValueError:
                length = 0

            try:
                raw = self.rfile.read(length).decode("utf-8") or "{}"
                payload = json.loads(raw)
            except (UnicodeDecodeError, json.JSONDecodeError):
                self._send_json(400, {"ok": False, "error": "invalid JSON body"})
                return

            query = payload.get("query", "")
            if not isinstance(query, str) or not query.strip():
                self._send_json(400, {"ok": False, "error": "'query' must be a non-empty string"})
                return
            if "\n" in query or "\r" in query:
                self._send_json(400, {"ok": False, "error": "'query' must be one line"})
                return

            try:
                output = process.query(query)
            except RuntimeError as exc:
                self._send_json(500, {"ok": False, "error": str(exc)})
                return

            self._send_json(200, {"ok": True, "output": output, "error": "", "returncode": 0})

    return Handler


def console_loop(process):
    if sys.stdin is None:
        return
    for raw_line in sys.stdin:
        query = raw_line.rstrip("\r\n")
        if not query.strip():
            continue
        try:
            output = process.query(query)
            sys.stdout.write(output)
            sys.stdout.flush()
        except RuntimeError as exc:
            sys.stderr.write(f"console error: {exc}\n")
            sys.stderr.flush()


def main():
    parser = argparse.ArgumentParser(
        description="Run a query-response program behind a single-threaded HTTP server.",
        usage="server.py [--port-start PORT_START] [--port-end PORT_END] --cmd ARG [ARG ...]",
    )
    parser.add_argument("--port-start", type=int, default=8000, help="First port in the auto-bind range.")
    parser.add_argument("--port-end", type=int, default=8999, help="Last port in the auto-bind range.")
    parser.add_argument(
        "--cmd",
        nargs=argparse.REMAINDER,
        required=True,
        metavar="ARG",
        help="Program and all following arguments.",
    )
    args = parser.parse_args()

    if args.port_start > args.port_end:
        parser.error("--port-start must not be greater than --port-end")

    command = args.cmd
    if not command:
        parser.error("--cmd must not be empty")

    process = QueryResponseProcess(command)
    handler = make_handler(process)
    httpd = None
    for port in range(args.port_start, args.port_end + 1):
        try:
            httpd = HTTPServer((HOST, port), handler)
            break
        except OSError:
            continue

    if httpd is None:
        process.close()
        parser.exit(1, f"server.py: no free port in {args.port_start}-{args.port_end}\n")

    print(f"http://{HOST}:{httpd.server_address[1]}/", flush=True)

    console_thread = threading.Thread(target=console_loop, args=(process,), daemon=True)
    console_thread.start()

    try:
        httpd.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        httpd.server_close()
        process.close()


if __name__ == "__main__":
    main()
