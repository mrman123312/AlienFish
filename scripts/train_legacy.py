"""Explore all qualifying opening branches, with SQLite checkpoints and two searches."""
from __future__ import annotations

import argparse
import json
import sqlite3
from pathlib import Path

from alien_uci import Engine


def qualified(result, advantage, loss, enforce_advantage):
    # A decisive best score is a separate outcome, not a centipawn baseline.
    # Never discard a proven mate and compare inferior numerical alternatives.
    if result["moves"] and "cp" not in result["moves"][0]:
        return {}
    exact = [m for m in result["moves"] if m["exact"] and "cp" in m]
    if not exact or result["repeated"]:
        return {}
    best = max(m["cp"] for m in exact)
    return {m["move"]: m for m in exact if best - m["cp"] <= loss
            and (not enforce_advantage or m["cp"] >= advantage)}


def train(args):
    db_path = Path(args.frontier).resolve()
    db_path.parent.mkdir(parents=True, exist_ok=True)
    bank = str(Path(args.bank).resolve())
    config = {key: getattr(args, key) for key in (
        "fen", "side", "depth", "verify_depth", "advantage", "max_loss", "max_plies"
    )}
    config["bank"] = bank
    with Engine(args.engine, timeout=args.timeout) as engine, sqlite3.connect(db_path) as db:
        engine.option("AlienFish Mode", "Normal")
        engine.option("AlienLegacy", "false")  # Avoid circular evidence from old advice
        engine.option("AlienLegacy Learning", "false")
        engine.option("AlienLegacy File", bank)
        engine.option("AlienLegacy Min Depth", args.verify_depth)
        engine.option("AlienLegacy Min Advantage", args.advantage)
        engine.option("AlienLegacy Max Loss", args.max_loss)
        engine.option("AlienLegacy Capacity", args.capacity)
        engine.option("MultiPV", 256)
        engine.option("Threads", args.threads)
        engine.option("Hash", args.hash)
        config["network"] = engine.legacy("status")["network"]
        db.executescript("""
          PRAGMA journal_mode=WAL;
          CREATE TABLE IF NOT EXISTS metadata (key TEXT PRIMARY KEY, value TEXT NOT NULL);
          CREATE TABLE IF NOT EXISTS jobs (
            id INTEGER PRIMARY KEY, position_key TEXT UNIQUE NOT NULL,
            moves TEXT NOT NULL, ply INTEGER NOT NULL, status TEXT NOT NULL DEFAULT 'pending'
          );
          CREATE TABLE IF NOT EXISTS edges (
            parent_key TEXT, move TEXT, child_key TEXT, cp INTEGER, depth INTEGER,
            PRIMARY KEY(parent_key, move)
          );
        """)
        encoded = json.dumps(config, sort_keys=True)
        existing = db.execute("SELECT value FROM metadata WHERE key='config'").fetchone()
        if existing and existing[0] != encoded:
            raise ValueError("Frontier configuration/network differs. Use a new frontier filename.")
        db.execute("INSERT OR IGNORE INTO metadata VALUES ('config', ?)", (encoded,))
        # One trainer per frontier. A terminated job is replayed safely on restart.
        db.execute("UPDATE jobs SET status='pending' WHERE status='running'")
        engine.position(args.fen)
        root = engine.legacy("candidates")
        db.execute("INSERT OR IGNORE INTO jobs(position_key,moves,ply) VALUES (?,?,0)",
                   (root["key"], "[]"))
        db.commit()
        completed = 0
        while completed < args.max_positions:
            job = db.execute("SELECT id,position_key,moves,ply FROM jobs WHERE status='pending' "
                             "ORDER BY ply,id LIMIT 1").fetchone()
            if not job:
                break
            ident, position_key, encoded_moves, ply = job
            db.execute("UPDATE jobs SET status='running' WHERE id=?", (ident,))
            db.commit()
            moves = json.loads(encoded_moves)
            try:
                first = engine.analyse_all(args.fen, moves, args.depth)
                verified = engine.analyse_all(args.fen, moves, args.verify_depth)
                turn = verified["fen"].split()[1]
                enforce = args.side == "both" or turn == args.side[0]
                preliminary = qualified(first, args.advantage, args.max_loss, enforce)
                final = qualified(verified, args.advantage, args.max_loss, enforce)
                accepted = [m for m in final if m in preliminary]
                # Publish only explicitly approved moves from the finished,
                # unrestricted verification search. The engine reapplies its gates.
                if accepted:
                    engine.legacy("learn " + " ".join(accepted))
                for move in accepted:
                    child = engine.legacy("child " + move)
                    score = final[move]
                    db.execute("INSERT OR REPLACE INTO edges VALUES (?,?,?,?,?)", (
                        position_key, move, child["key"], score["cp"], score["depth"]
                    ))
                    if ply + 1 < args.max_plies:
                        db.execute("INSERT OR IGNORE INTO jobs(position_key,moves,ply) VALUES (?,?,?)", (
                            child["key"], json.dumps(moves + [move]), ply + 1
                        ))
                db.execute("UPDATE jobs SET status='done' WHERE id=?", (ident,))
                db.commit()
            except BaseException:
                db.rollback()
                db.execute("UPDATE jobs SET status='pending' WHERE id=?", (ident,))
                db.commit()
                raise
            completed += 1
            waiting = db.execute("SELECT COUNT(*) FROM jobs WHERE status='pending'").fetchone()[0]
            print(json.dumps({"processed": completed, "ply": ply, "accepted": len(accepted),
                              "frontier": waiting, "bank": engine.legacy("status")}), flush=True)
        waiting = db.execute("SELECT COUNT(*) FROM jobs WHERE status='pending'").fetchone()[0]
        print(json.dumps({"run_finished": True, "frontier_remaining": waiting, "processed_this_run": completed,
                          "bank": engine.legacy("status")}), flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--engine", default="src/stockfish")
    parser.add_argument("--bank", default="AlienLegacy.afl")
    parser.add_argument("--frontier", default="AlienLegacy-frontier.sqlite3")
    parser.add_argument("--fen", help="Root FEN; default is the normal starting position")
    parser.add_argument("--side", choices=("white", "black", "both"), default="white",
                        help="Apply the advantage floor to this repertoire side; explore good opponent replies too")
    parser.add_argument("--depth", type=int, default=10)
    parser.add_argument("--verify-depth", type=int, default=14)
    parser.add_argument("--advantage", type=int, default=0)
    parser.add_argument("--max-loss", type=int, default=20)
    parser.add_argument("--max-positions", type=int, default=1000)
    parser.add_argument("--max-plies", type=int, default=24)
    parser.add_argument("--capacity", type=int, default=1000000)
    parser.add_argument("--threads", type=int, default=1)
    parser.add_argument("--hash", type=int, default=256)
    parser.add_argument("--timeout", type=int, default=1800)
    args = parser.parse_args()
    if not (1 <= args.depth < args.verify_depth <= 54 and 0 <= args.advantage <= 2000
            and 0 <= args.max_loss <= 200 and 1 <= args.threads <= 1024
            and 1 <= args.capacity <= 5000000 and 1 <= args.hash <= 65536
            and args.max_positions > 0 and args.max_plies > 0 and args.timeout > 0):
        parser.error("Invalid search depth, threshold, capacity or resource limit")
    train(args)


if __name__ == "__main__":
    main()
