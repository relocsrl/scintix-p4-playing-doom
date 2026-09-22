#!/usr/bin/env python3
"""
Test doubles for doom_jev_agent.py — run the whole pipeline with no API keys:

  * a mock TypeSafe endpoint  POST /v1/systemone   (port 8899)
    - validates the request shape the agent sends
    - answers with the exact response schema, using the agent's DryJev policy
    - adds ~120 ms latency; every 25th call returns 429 to exercise the retry
  * a mock LLM supervisor
    - Anthropic Messages   POST /v1/messages          (port 8898)
    - OpenAI Chat          POST /v1/chat/completions  (same port)
    - checks headers, returns a JSON intent (random heading) as the model text

Ports: 8899 (Jev) and 8898 (LLM), or `python tests/mock_servers.py <jev_port> <llm_port>`.

Run (from tools/):
    python doom_agent_sim.py                      # terminal 1: fake device
    python tests/mock_servers.py                  # terminal 2: fake Jev + fake LLM
    python doom_jev_agent.py localhost --port 8765 --jev-url http://127.0.0.1:8899/v1/systemone \
        --jev-key test-key-123456 --mode supervised --supervisor openai \
        --supervisor-base-url http://127.0.0.1:8898/v1 --supervisor-key sk-test --steps 100
"""

import json
import os
import random
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))  # tools/
import doom_jev_agent as agent  # noqa: E402

policy = agent.DryJev()
counter = {"jev": 0, "llm": 0, "bad": 0}
rng = random.Random(7)


class JevHandler(BaseHTTPRequestHandler):
    def log_message(self, *a):
        pass

    def _fail(self, code, msg):
        counter["bad"] += 1
        body = json.dumps({"error": msg}).encode()
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)
        print(f"[mock-jev] {code} {msg}", flush=True)

    def do_POST(self):
        if self.path != "/v1/systemone":
            return self._fail(404, "unknown path " + self.path)
        auth = self.headers.get("Authorization", "")
        if not auth.startswith("Bearer ") or len(auth) < 12:
            return self._fail(401, "bad auth")
        n = int(self.headers.get("Content-Length", 0))
        try:
            req = json.loads(self.rfile.read(n))
        except json.JSONDecodeError:
            return self._fail(422, "bad json")
        for key in ("model", "state", "questions"):
            if key not in req:
                return self._fail(422, f"missing {key}")
        qs = req["questions"]
        if not isinstance(qs, dict) or "action" not in qs or "fire" not in qs:
            return self._fail(422, "questions must include action/fire")
        if qs["action"].get("type") != "choice" or not isinstance(qs["action"].get("criteria"), dict):
            return self._fail(422, "action must be a choice with criteria map")
        if len(qs["action"]["criteria"]) > 255:
            return self._fail(422, "too many options")
        if qs["fire"].get("type") != "noul" or "instructions" not in qs["fire"]:
            return self._fail(422, "fire must be a noul with instructions")
        state = req["state"]
        if not isinstance(state, dict) or "view" not in state or "player" not in state:
            return self._fail(422, "state must be the agent's state object")

        counter["jev"] += 1
        if counter["jev"] % 25 == 0:
            return self._fail(429, "rate limited (synthetic)")
        time.sleep(0.12)
        answers, _ = policy.decide(state, qs)
        # emulate a calibrated, slightly noisy model
        a = answers["action"]
        a["confidence"] = round(rng.uniform(0.3, 0.9), 2)
        tokens = len(json.dumps(req)) // 4
        body = json.dumps({"model": "jev-1.13.0", "answers": answers,
                           "usage": {"input_tokens": tokens, "output_tokens": 20}}).encode()
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)


class LLMHandler(BaseHTTPRequestHandler):
    def log_message(self, *a):
        pass

    def _send(self, code, obj):
        body = json.dumps(obj).encode()
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_POST(self):
        n = int(self.headers.get("Content-Length", 0))
        req = json.loads(self.rfile.read(n))
        counter["llm"] += 1
        heading = rng.choice([0, 45, 90, 135, 180, 225, 270, 315])
        intent = {"goal": f"Mock plan #{counter['llm']}: head {agent.compass_name(heading)} and open doors on the way.",
                  "target_heading": heading, "fire_policy": rng.choice(["normal", "aggressive"]),
                  "preferred_weapon": None, "note": "mock supervisor"}
        text = "Here is the plan:\n" + json.dumps(intent)   # prose around JSON, on purpose
        time.sleep(0.8)
        if self.path == "/v1/messages":
            if not self.headers.get("x-api-key") or not self.headers.get("anthropic-version"):
                counter["bad"] += 1
                return self._send(401, {"error": "missing anthropic headers"})
            if "system" not in req or "messages" not in req or "model" not in req:
                counter["bad"] += 1
                return self._send(400, {"error": "bad anthropic body"})
            user = req["messages"][0]["content"]
            assert "automap" in user and "Replan trigger" in user, "prompt missing expected sections"
            return self._send(200, {"id": "msg_mock", "type": "message", "role": "assistant", "model": req["model"],
                                    "content": [{"type": "text", "text": text}],
                                    "usage": {"input_tokens": len(user) // 4, "output_tokens": 60}})
        if self.path == "/v1/chat/completions":
            if not self.headers.get("Authorization", "").startswith("Bearer "):
                counter["bad"] += 1
                return self._send(401, {"error": "missing bearer"})
            if req["messages"][0]["role"] != "system":
                counter["bad"] += 1
                return self._send(400, {"error": "expected system message first"})
            return self._send(200, {"id": "chatcmpl-mock", "object": "chat.completion", "model": req["model"],
                                    "choices": [{"index": 0, "message": {"role": "assistant", "content": text},
                                                 "finish_reason": "stop"}]})
        counter["bad"] += 1
        return self._send(404, {"error": "unknown path " + self.path})


def main():
    jev_port = int(sys.argv[1]) if len(sys.argv) > 1 else 8899
    llm_port = int(sys.argv[2]) if len(sys.argv) > 2 else 8898
    jev = ThreadingHTTPServer(("127.0.0.1", jev_port), JevHandler)
    llm = ThreadingHTTPServer(("127.0.0.1", llm_port), LLMHandler)
    threading.Thread(target=jev.serve_forever, daemon=True).start()
    threading.Thread(target=llm.serve_forever, daemon=True).start()
    print(f"[mock] jev on :{jev_port}/v1/systemone, llm on :{llm_port} (/v1/messages, /v1/chat/completions)",
          flush=True)
    try:
        while True:
            time.sleep(5)
            print(f"[mock] calls: jev={counter['jev']} llm={counter['llm']} rejected={counter['bad']}", flush=True)
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
