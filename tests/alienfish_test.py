"""Integration tests for AlienFish's UCI modes, persistence and exploration."""
import argparse
import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))
from alien_uci import Engine

EXECUTABLE = ROOT / "src" / "stockfish"


class AlienFishTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.path = Path(self.temp.name)
        self.engine = Engine(EXECUTABLE, cwd=self.path, timeout=60)
        self.engine.option("AlienLegacy File", self.path / "bank.afl")
        self.engine.option("AlienLegacy Min Depth", 6)
        self.engine.option("Hash", 16)

    def tearDown(self):
        self.engine.close()
        self.temp.cleanup()

    def test_persistence_and_compaction(self):
        self.engine.position()
        self.engine.go("depth 10")
        status = self.engine.legacy("status")
        self.assertGreater(status["records"], 0)
        self.assertEqual(status["pending"], 0)
        self.assertTrue((self.path / "bank.afl").exists())
        self.engine.close()
        self.engine = Engine(EXECUTABLE, cwd=self.path)
        self.engine.option("AlienLegacy File", self.path / "bank.afl")
        self.assertEqual(status["records"], self.engine.legacy("status")["records"])
        self.assertEqual(status["records"], self.engine.legacy("compact")["records"])

    def test_restricted_search_never_trains(self):
        self.engine.position()
        self.engine.go("depth 10 searchmoves e2e4")
        self.assertEqual(self.engine.legacy("status")["records"], 0)
        with self.assertRaises(RuntimeError):
            self.engine.legacy("learn e2e4")

    def test_all_legal_moves_and_manual_learning(self):
        self.engine.option("AlienLegacy Learning", "false")
        self.engine.option("MultiPV", 256)
        result = self.engine.analyse_all(None, [], 6)
        self.assertEqual(result["legal_count"], 20)
        self.assertEqual(len(result["moves"]), 20)
        self.assertTrue(all(m["exact"] and m["depth"] >= 6 for m in result["moves"]))
        self.assertEqual(self.engine.legacy("status")["records"], 0)
        good = [m for m in result["moves"] if "cp" in m and m["cp"] >= 0]
        self.assertTrue(good)
        chosen = max(good, key=lambda m: m["cp"])["move"]
        status = self.engine.legacy("learn " + chosen)
        self.assertEqual(status["records"], 1)
        child = self.engine.legacy("child " + chosen)
        self.assertNotEqual(result["key"], child["key"])

    def test_gambit_prefix_transposition_and_deviation(self):
        self.engine.option("AlienFish Mode", "Brilliant Legacy")
        prefix = []
        line = ["e2e4", "c7c6", "d2d4", "d7d5", "b1c3", "d5e4", "c3e4", "g8f6",
                "e4g5", "h7h6", "g5f7", "e8f7", "g1f3"]
        for ply, expected in enumerate(line):
            if ply % 2 == 0:
                self.engine.position(moves=prefix)
                response = self.engine.go("depth 6")
                self.assertEqual(response[-1].split()[1], expected)
                self.assertTrue(any("Alien Gambit opening choice" in s for s in response))
            prefix.append(expected)
        self.assertEqual(self.engine.legacy("status")["records"], 0)
        self.engine.position(moves=["e2e4", "c7c6", "d2d4", "d7d5", "b1d2", "d5e4",
                                    "d2e4", "g8f6", "e4g5", "h7h6"])
        self.assertEqual(self.engine.go("depth 6")[-1].split()[1], "g5f7")
        self.engine.position(moves=["e2e4", "e7e5"])
        response = self.engine.go("depth 6")
        self.assertFalse(any("Alien Gambit opening choice" in s for s in response))
        self.engine.position(moves=["e2e4", "c7c6", "d2d4", "d7d5", "b1c3", "d5e4",
                                    "c3e4", "g8f6", "e4g5", "h7h6"])
        response = self.engine.go("depth 6 searchmoves g5e4")
        self.assertEqual(response[-1].split()[1], "g5e4")

    def test_chess960_and_black_are_not_forced_into_gambit(self):
        self.engine.option("AlienFish Mode", "Brilliant Legacy")
        self.engine.option("UCI_Chess960", "true")
        self.engine.position()
        response = self.engine.go("depth 6")
        self.assertFalse(any("Alien Gambit opening choice" in s for s in response))
        self.engine.option("UCI_Chess960", "false")
        self.engine.position(moves=["e2e4"])
        response = self.engine.go("depth 6")
        self.assertFalse(any("Alien Gambit opening choice" in s for s in response))

    def test_stop_mate_stalemate_and_repetition(self):
        self.engine.option("AlienLegacy Min Depth", 64)
        self.engine.option("AlienFish Mode", "Brilliant!")
        self.engine.position()
        self.engine.send("go infinite")
        self.engine.send("stop")
        response = self.engine.until(lambda s: s.startswith("bestmove "))
        self.assertNotEqual(response[-1].split()[1], "0000")
        self.assertEqual(self.engine.legacy("status")["records"], 0)
        for fen in ("7k/6Q1/6K1/8/8/8/8/8 b - - 0 1",
                    "7k/5Q2/6K1/8/8/8/8/8 b - - 0 1"):
            self.engine.position(fen)
            self.assertIn(self.engine.go("depth 6")[-1].split()[1], ("0000", "(none)"))
            self.assertEqual(self.engine.legacy("candidates")["moves"], [])
        self.engine.position(moves=["g1f3", "g8f6", "f3g1", "f6g8"])
        self.assertTrue(self.engine.legacy("candidates")["repeated"])

    def test_multi_threaded_search_finishes(self):
        self.engine.option("AlienLegacy Learning", "false")
        self.engine.option("Threads", 2)
        self.engine.option("AlienFish Mode", "Brilliant!")
        self.engine.position()
        response = self.engine.go("nodes 20000")
        self.assertNotEqual(response[-1].split()[1], "0000")
        self.engine.ready()

    def test_explorer_resume(self):
        args = [sys.executable, str(ROOT / "scripts" / "train_legacy.py"),
                "--engine", str(EXECUTABLE), "--bank", str(self.path / "explorer.afl"),
                "--frontier", str(self.path / "frontier.sqlite3"), "--depth", "3",
                "--verify-depth", "5", "--hash", "16", "--max-positions", "1", "--max-plies", "4"]
        first = subprocess.run(args, check=True, capture_output=True, text=True, cwd=self.path, timeout=90)
        second = subprocess.run(args, check=True, capture_output=True, text=True, cwd=self.path, timeout=90)
        self.assertEqual(json.loads(first.stdout.splitlines()[-1])["processed_this_run"], 1)
        self.assertEqual(json.loads(second.stdout.splitlines()[-1])["processed_this_run"], 1)
        self.assertTrue((self.path / "frontier.sqlite3").exists())


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--engine", type=Path, default=EXECUTABLE)
    args, unittest_args = parser.parse_known_args()
    EXECUTABLE = args.engine.resolve()
    unittest.main(argv=[sys.argv[0]] + unittest_args)
