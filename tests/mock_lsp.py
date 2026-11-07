#!/usr/bin/env python3
"""A tiny fake language server for testing the LSP client (stdio JSON-RPC)."""
import json, sys

def read():
    headers = {}
    while True:
        line = sys.stdin.buffer.readline()
        if not line:
            return None
        line = line.decode().strip()
        if not line:
            break
        k, v = line.split(":", 1)
        headers[k.lower()] = v.strip()
    n = int(headers["content-length"])
    return json.loads(sys.stdin.buffer.read(n))

def send(obj):
    body = json.dumps(obj).encode()
    sys.stdout.buffer.write(b"Content-Length: %d\r\n\r\n" % len(body) + body)
    sys.stdout.buffer.flush()

while True:
    msg = read()
    if msg is None:
        break
    method = msg.get("method")
    mid = msg.get("id")
    if method == "initialize":
        send({"jsonrpc": "2.0", "id": mid, "result": {"capabilities": {"completionProvider": {}}}})
        # server -> client request: the client must answer it
        send({"jsonrpc": "2.0", "id": 900, "method": "window/workDoneProgress/create", "params": {"token": "t"}})
    elif method == "textDocument/didOpen":
        uri = msg["params"]["textDocument"]["uri"]
        text = msg["params"]["textDocument"]["text"]
        send({"jsonrpc": "2.0", "method": "textDocument/publishDiagnostics", "params": {
            "uri": uri, "diagnostics": [
                {"range": {"start": {"line": 0, "character": 2}, "end": {"line": 0, "character": 5}},
                 "severity": 1, "message": "unknown type é 'foo'", "source": "clang", "code": "unknown_typename"},
                {"range": {"start": {"line": 1, "character": 0}, "end": {"line": 1, "character": 1}},
                 "severity": 2, "message": "unused variable"}]}})
    elif method == "textDocument/didChange":
        uri = msg["params"]["textDocument"]["uri"]
        text = msg["params"]["contentChanges"][0]["text"]
        send({"jsonrpc": "2.0", "method": "textDocument/publishDiagnostics", "params": {
            "uri": uri, "diagnostics": [] if "ok" in text else [
                {"range": {"start": {"line": 0, "character": 0}, "end": {"line": 0, "character": 1}},
                 "severity": 1, "message": "changed:" + str(len(text))}]}})
    elif method == "textDocument/completion":
        send({"jsonrpc": "2.0", "id": mid, "result": {"isIncomplete": False, "items": [
            {"label": " push_back", "kind": 2, "detail": "void", "sortText": "40", "filterText": "push_back", "insertText": "push_back"},
            {"label": " size", "kind": 2, "detail": "size_t", "textEdit": {"newText": "size", "range": {}}},
            {"label": " value", "kind": 5}]}})
    elif method == "textDocument/definition":
        send({"jsonrpc": "2.0", "id": mid, "result": [
            {"uri": "file:///tmp/a%20b/main.cpp", "range": {"start": {"line": 4, "character": 7}, "end": {"line": 4, "character": 9}}}]})
    elif method == "textDocument/hover":
        send({"jsonrpc": "2.0", "id": mid, "result": {"contents": {"kind": "markdown", "value": "int x"}}})
    elif method == "textDocument/switchSourceHeader":
        send({"jsonrpc": "2.0", "id": mid, "result": "file:///proj/src/main.h"})
    elif method == "shutdown":
        send({"jsonrpc": "2.0", "id": mid, "result": None})
    elif method == "exit":
        break
    elif mid is not None:
        send({"jsonrpc": "2.0", "id": mid, "error": {"code": -32601, "message": "unknown"}})
