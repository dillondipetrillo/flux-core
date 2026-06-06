#! /usr/bin/env python3

"""
Reference HTTP auth service for the C State Bus engine

This shows the exact request/response format the engine expects.
Copy the validate_token() function into your existing server rather than
running this as a standalone service.

Usage (standalone for testing):
    python3 auth_service.py

Engine configuration:
    ENGINE_AUTH_HTTP_URL=http://localhost:8888/validate ./server

Request from engine:
    POST /validate
    Content-Type: application/json
    {"token": "<base64-encoded token>"}
    
Your response:
    {"valid": true, "user_id": 42} on success
    {"valid": false, "user_id": 0} on failure
"""
import base64
import hashlib
import json
import os
from http.server import BaseHTTPRequestHandler, HTTPServer

def validate_token(token_b64: str) -> tuple[bool, int]:
    """
    Replace this with your own token validation logic.
    
    Examples:
        - Verify a JWT using python-jose or PyJWT
        - Look up a session token in your database
        - Check an API key in your Redis cache
        - Call your OAuth introspection endpoint
    """
    try:
        token_bytes = base64.b64decode(token_b64 + "==")
        token = token_bytes.decode("utf-8", errors="replace")
    except Exception:
        return False, 0
    if not token:
        return False, 0
    
    # Default: accept any non-empty token for development
    # Derive a consistent user_id from the token
    uid = int.from_bytes(hashlib.sha256(token_bytes).digest()[:4], "big") or 1
    return True, uid

class AuthHandler(BaseHTTPRequestHandler):
    def do_POST(self):
        if self.path != "/validate":
            self.send_response(404)
            self.end_headers()
            return
        length = int(self.headers.get("Content-Length", 0))
        try:
            data = json.loads(self.rfile.read(length))
            valid, user_id = validate_token(data.get("token", ""))
        except Exception as e:
            print(f"Error: {e}")
            valid, user_id = False, 0
            
        body = json.dumps({"valid": valid, "user_id": user_id}).encode()
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)
        
    def do_GET(self):
        if self.path == "/health":
            self.send_response(200)
            self.end_headers()
            self.wfile.write(b"OK")
        else:
            self.send_response(404)
            self.end_headers()
            
    def log_message(self, fmt, *args):
        pass


if __name__ == "__main__":
    port = int(os.environ.get("PORT", 8888))
    print(f"Reference auth service on port {port}")
    print(f"ENGINE_AUTH_HTTP_URL=http://localhost:{port}/validate")
    print("Replace validate_token() with your own logic.")
    HTTPServer(("", port), AuthHandler).serve_forever()