#!/usr/bin/env python3
import argparse
import socket
from http.server import BaseHTTPRequestHandler, HTTPServer
from pathlib import Path


HTTP_RESPONSE_MAX = 2 * 1024 * 1024


class FixtureHandler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, format, *args):
        return

    def send_body(self, status, body, extra_headers=None):
        self.send_response(status)
        self.send_header("Content-Type", "text/plain")
        self.send_header("Content-Length", str(len(body)))
        for name, value in extra_headers or []:
            self.send_header(name, value)
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        if self.path == "/get":
            valid = (
                self.headers.get("X-Fixture") == "get-header"
                and self.headers.get("Host") == "fixture.example"
                and self.headers.get("Accept-Encoding") == "identity"
            )
            status = 200 if valid else 400
            self.send_body(status, b"GET_OK" if status == 200 else b"GET_HEADER_BAD")
            return
        if self.path == "/redirect":
            self.send_response(302)
            self.send_header("Location", "/redirect-target")
            self.send_header("Content-Length", "0")
            self.end_headers()
            return
        if self.path == "/redirect-target":
            valid = (
                self.headers.get("X-Fixture") == "redirect-header"
                and self.headers.get("Host") == "fixture.example"
                and self.headers.get("Accept-Encoding") == "identity"
            )
            status = 200 if valid else 400
            self.send_body(status, b"REDIRECT_OK" if status == 200 else b"REDIRECT_HEADER_BAD")
            return
        if self.path == "/status/404":
            self.send_body(404, b"STATUS_404")
            return
        if self.path == "/status/500":
            self.send_body(500, b"STATUS_500")
            return
        if self.path == "/close":
            self.close_connection = True
            try:
                self.connection.shutdown(socket.SHUT_RDWR)
            except OSError:
                pass
            self.connection.close()
            return
        if self.path == "/large":
            self.send_response(200)
            self.send_header("Content-Type", "application/octet-stream")
            self.send_header("Content-Length", str(HTTP_RESPONSE_MAX + 1))
            self.end_headers()
            chunk = b"x" * 65536
            remaining = HTTP_RESPONSE_MAX + 1
            try:
                while remaining:
                    piece = chunk if remaining >= len(chunk) else chunk[:remaining]
                    self.wfile.write(piece)
                    remaining -= len(piece)
            except (BrokenPipeError, ConnectionResetError):
                pass
            return
        self.send_body(404, b"UNKNOWN_PATH")

    def do_POST(self):
        length = int(self.headers.get("Content-Length", "0"))
        body = self.rfile.read(length)
        if self.path == "/post-json":
            expected = b'{"fixture":"post","value":"alpha"}'
            valid = self.headers.get("X-Fixture") == "post-json" and body == expected
            self.send_body(200 if valid else 400, b"POST_JSON_OK" if valid else b"POST_JSON_BAD")
            return
        if self.path == "/post-form":
            expected = b"grant_type=refresh_token&refresh_token=a%2Bb"
            valid = self.headers.get("X-Fixture") == "post-form" and body == expected
            self.send_body(200 if valid else 400, b"POST_FORM_OK" if valid else b"POST_FORM_BAD")
            return
        self.send_body(404, b"UNKNOWN_PATH")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--port-file", required=True)
    args = parser.parse_args()
    server = HTTPServer(("127.0.0.1", 0), FixtureHandler)
    Path(args.port_file).write_text(str(server.server_port), encoding="ascii")
    server.serve_forever()


if __name__ == "__main__":
    main()
