"""Small, dependency-free UCI client shared by AlienFish tools and tests."""
from __future__ import annotations

import json
import queue
import subprocess
import threading
import time
from pathlib import Path


class Engine:
    def __init__(self, executable, cwd=None, timeout=300):
        self.timeout = timeout
        self.process = subprocess.Popen(
            [str(Path(executable).resolve())], cwd=cwd, stdin=subprocess.PIPE,
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, bufsize=1,
        )
        self.lines = queue.Queue()
        self.reader = threading.Thread(target=self._read, daemon=True)
        self.reader.start()
        self.send("uci")
        self.banner = self.until(lambda line: line == "uciok")
        if not any("option name AlienFish Mode " in line for line in self.banner):
            self.close()
            raise RuntimeError("The selected executable is not an AlienFish build")

    def _read(self):
        for line in self.process.stdout:
            self.lines.put(line.rstrip("\r\n"))
        self.lines.put(None)

    def send(self, text):
        if "\n" in text or "\r" in text:
            raise ValueError("UCI commands must occupy one line")
        if self.process.poll() is not None:
            raise RuntimeError(f"Engine exited with code {self.process.returncode}")
        self.process.stdin.write(text + "\n")
        self.process.stdin.flush()

    def until(self, predicate, timeout=None):
        deadline = time.monotonic() + (self.timeout if timeout is None else timeout)
        result = []
        while True:
            try:
                line = self.lines.get(timeout=max(0.001, deadline - time.monotonic()))
            except queue.Empty as exc:
                raise TimeoutError("Engine response timed out") from exc
            if line is None:
                raise RuntimeError("Engine closed its output:\n" + "\n".join(result[-15:]))
            result.append(line)
            if predicate(line):
                return result
            if time.monotonic() >= deadline:
                raise TimeoutError("Engine response timed out")

    def ready(self):
        self.send("isready")
        self.until(lambda line: line == "readyok")

    def option(self, name, value):
        self.send(f"setoption name {name} value {value}")
        self.ready()

    def position(self, fen=None, moves=()):
        self.send(("position startpos" if fen is None else "position fen " + fen)
                  + (" moves " + " ".join(moves) if moves else ""))
        self.ready()

    def go(self, limits):
        self.send("go " + limits)
        return self.until(lambda line: line.startswith("bestmove "))

    def legacy(self, command):
        self.send("legacy " + command)
        result = self.until(lambda line: line.startswith("info string {")
                            or line.startswith("info string AlienLegacy:"))[-1]
        if not result.startswith("info string {"):
            raise RuntimeError(result)
        return json.loads(result.removeprefix("info string "))

    def analyse_all(self, fen, moves, depth):
        """Insist on a complete all-legal-move iteration at the actual depth."""
        self.send("ucinewgame")
        self.position(fen, moves)
        # Aspiration retries can reduce nominal depth. Check the actual depth
        # exported by the engine, and deepen again when necessary.
        for nominal in range(depth + 2, min(depth + 10, 65), 2):
            self.go(f"depth {nominal}")
            result = self.legacy("candidates")
            if not result["legal_count"]:
                return result
            if len(result["moves"]) == result["legal_count"] and all(
                move["exact"] and move["depth"] >= depth for move in result["moves"]
            ):
                return result
        raise RuntimeError(f"Incomplete all-move search at actual depth {depth}; node will be retried")

    def close(self):
        if self.process.poll() is None:
            try:
                self.send("stop")
                self.send("quit")
                self.process.wait(timeout=15)
            except (OSError, RuntimeError, subprocess.TimeoutExpired):
                self.process.kill()
                self.process.wait()
        for stream in (self.process.stdin, self.process.stdout):
            if stream:
                stream.close()

    def __enter__(self):
        return self

    def __exit__(self, *args):
        self.close()
