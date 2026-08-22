#!/usr/bin/env python3
"""Send one-line query instructions to a server.py endpoint."""

import argparse
import json
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


def main():
    parser = argparse.ArgumentParser(description="Send one-line queries to a server.py server.")
    parser.add_argument("url", help="URL printed by server.py")
    parser.add_argument(
        "query",
        nargs="?",
        help="One-line query; if omitted, one line is read from stdin.",
    )
    parser.add_argument("--timeout", type=float, default=60, help="HTTP timeout in seconds")
    args = parser.parse_args()

    query = args.query
    if query is None:
        query = sys.stdin.readline()
        if query == "":
            parser.error("missing query; pass it as an argument or pipe one line to stdin")
        query = query.rstrip("\r\n")

    if "\n" in query or "\r" in query:
        parser.error("query must be a single line")

    try:
        result = send_query(args.url, query, args.timeout)
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
        print(f"client.py: request timed out after {args.timeout}s", file=sys.stderr)
        sys.exit(1)

    if result.get("output"):
        print(result["output"], end="")
    if result.get("error"):
        print(result["error"], end="", file=sys.stderr)
    if not result.get("ok"):
        sys.exit(result.get("returncode") or 1)


if __name__ == "__main__":
    main()
