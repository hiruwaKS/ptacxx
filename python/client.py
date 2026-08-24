#!/usr/bin/env python3
"""Send one-line query instructions to a server.py endpoint using environment variables."""

import json
import os
import sys
import urllib.error
import urllib.request


def send_query(url, query, timeout):
    endpoint = url.rstrip("/") + "/query"
    body = json.dumps({"query": query}, ensure_ascii=False).encode("utf-8")
    request = urllib.request.Request(
        endpoint,
        data=body,
        headers={"Content-Type": "application/json"},
        method="POST",
    )
    with urllib.request.urlopen(request, timeout=timeout) as response:
        return json.loads(response.read().decode("utf-8"))


def encode_if_necessary(s):
    if any(c in s for c in ' "\\'):
        encoded = ['"']
        for c in s:
            if c == '"' or c == '\\':
                encoded.append('\\')
            encoded.append(c)
        encoded.append('"')
        return ''.join(encoded)
    return s


def main():
    url = os.environ.get("SERVER_URL")
    timeout_str = os.environ.get("TIMEOUT", "5")
    
    if not url:
        print("client.py: SERVER_URL environment variable is required", file=sys.stderr)
        sys.exit(1)
    
    try:
        timeout = float(timeout_str)
    except ValueError:
        print(f"client.py: TIMEOUT must be a number, got '{timeout_str}'", file=sys.stderr)
        sys.exit(1)
    
    query = None
    if len(sys.argv) > 1:
        encoded_args = [encode_if_necessary(arg) for arg in sys.argv[1:]]
        query = " ".join(encoded_args)
    
    if query is None:
        query = sys.stdin.readline()
        if query == "":
            print("client.py: missing query; pass it as an argument or pipe one line to stdin", file=sys.stderr)
            sys.exit(1)
        query = query.rstrip("\r\n")
    
    if "\n" in query or "\r" in query:
        print("client.py: query must be a single line", file=sys.stderr)
        sys.exit(1)
    
    try:
        result = send_query(url, query, timeout)
    except urllib.error.HTTPError as exc:
        raw = exc.read().decode("utf-8", errors="replace")
        try:
            message = json.loads(raw).get("error", raw)
        except json.JSONDecodeError:
            message = raw or f"HTTP {exc.code}"
        print(message, file=sys.stderr)
        sys.exit(exc.code)
    except urllib.error.URLError as exc:
        print(f"client.py: cannot reach server: {exc.reason}", file=sys.stderr)
        sys.exit(1)
    except TimeoutError:
        print(f"client.py: request timed out after {timeout}s", file=sys.stderr)
        sys.exit(1)
    
    if result.get("output"):
        print(result["output"], end="")
    if result.get("error"):
        print(result["error"], end="", file=sys.stderr)
    if not result.get("ok"):
        sys.exit(result.get("returncode") or 1)


if __name__ == "__main__":
    main()
