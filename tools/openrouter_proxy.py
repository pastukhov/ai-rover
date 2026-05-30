#!/usr/bin/env python3
"""
OpenRouter HTTP proxy for AI Rover.

Cloudflare blocks large POST requests from ESP32's mbedTLS/lwIP stack.
This proxy accepts plain HTTP from the ESP32 on a local port and forwards
requests to OpenRouter over HTTPS from the host machine.

Usage:
    python3 tools/openrouter_proxy.py [--port 8080]

Then set OPENROUTER_PROXY_URL in include/secrets.h to:
    "http://<your-host-ip>:8080/api/v1/chat/completions"
"""

import argparse
import urllib.request
import urllib.error
from http.server import BaseHTTPRequestHandler, HTTPServer

UPSTREAM = "https://openrouter.ai"

class ProxyHandler(BaseHTTPRequestHandler):
    def log_message(self, fmt, *args):
        print(f"[proxy] {self.address_string()} {fmt % args}")

    def do_POST(self):
        length = int(self.headers.get("Content-Length", 0))
        body = self.rfile.read(length) if length > 0 else b""

        upstream_url = UPSTREAM + self.path
        headers = {
            "Content-Type": self.headers.get("Content-Type", "application/json"),
            "Authorization": self.headers.get("Authorization", ""),
            "Accept": self.headers.get("Accept", "application/json"),
        }

        req = urllib.request.Request(
            upstream_url,
            data=body,
            method="POST",
            headers=headers,
        )

        try:
            with urllib.request.urlopen(req, timeout=30) as resp:
                data = resp.read()
                self.send_response(resp.status)
                ct = resp.headers.get("Content-Type", "application/json")
                self.send_header("Content-Type", ct)
                self.send_header("Content-Length", str(len(data)))
                self.end_headers()
                self.wfile.write(data)
        except urllib.error.HTTPError as e:
            data = e.read()
            self.send_response(e.code)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(data)))
            self.end_headers()
            self.wfile.write(data)
        except Exception as e:
            self.send_response(502)
            msg = str(e).encode()
            self.send_header("Content-Length", str(len(msg)))
            self.end_headers()
            self.wfile.write(msg)

    def do_GET(self):
        upstream_url = UPSTREAM + self.path
        req = urllib.request.Request(upstream_url, method="GET")
        try:
            with urllib.request.urlopen(req, timeout=10) as resp:
                data = resp.read()
                self.send_response(resp.status)
                self.send_header("Content-Type", resp.headers.get("Content-Type", "application/json"))
                self.send_header("Content-Length", str(len(data)))
                self.end_headers()
                self.wfile.write(data)
        except Exception as e:
            self.send_response(502)
            self.end_headers()


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="OpenRouter HTTP proxy for ESP32")
    parser.add_argument("--port", type=int, default=8080)
    parser.add_argument("--host", default="0.0.0.0")
    args = parser.parse_args()

    server = HTTPServer((args.host, args.port), ProxyHandler)
    print(f"OpenRouter proxy listening on http://{args.host}:{args.port}")
    print(f"Set in secrets.h: OPENROUTER_PROXY_URL \"http://<host-ip>:{args.port}/api/v1/chat/completions\"")
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print("\nProxy stopped.")
