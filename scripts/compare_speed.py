"""Alternating fixed-depth benchmark. Reports timing; enforces matching node counts."""
import argparse
import json
import re
import statistics
import subprocess
import tempfile
from pathlib import Path


def bench(exe, depth):
    commands = ("setoption name AlienLegacy value false\n"
                "setoption name AlienLegacy Learning value false\n"
                f"bench 16 1 {depth} default depth\nquit\n")
    with tempfile.TemporaryDirectory() as cwd:
        run = subprocess.run([str(Path(exe).resolve())], input=commands, text=True,
                             capture_output=True, cwd=cwd, check=True, timeout=300)
    output = run.stdout + run.stderr
    fields = {}
    for key, label in (("nodes", "Nodes searched"), ("nps", "Nodes/second"), ("ms", "Total time \\(ms\\)")):
        values = re.findall(label + r"\s*:\s*(\d+)", output)
        if not values:
            raise RuntimeError(f"Missing benchmark field {key}")
        fields[key] = int(values[-1])
    return fields


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--baseline", required=True)
    parser.add_argument("--candidate", default="src/stockfish")
    parser.add_argument("--runs", type=int, default=5)
    parser.add_argument("--depth", type=int, default=14)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    if args.runs < 1 or args.depth < 1:
        parser.error("runs and depth must be positive")
    paths = {"baseline": args.baseline, "candidate": args.candidate}
    samples = {k: [] for k in paths}
    for path in paths.values():
        bench(path, min(args.depth, 10))
    for index in range(args.runs):
        order = ("baseline", "candidate") if index % 2 == 0 else ("candidate", "baseline")
        for name in order:
            samples[name].append(bench(paths[name], args.depth))
    nodes = {sample["nodes"] for values in samples.values() for sample in values}
    report = {"depth": args.depth, "runs": args.runs, "normal_search_nodes_match": len(nodes) == 1,
              "samples": samples, "median_nps": {k: statistics.median(s["nps"] for s in v)
                                                     for k, v in samples.items()},
              "note": "Local speed sample only; no Elo or Brilliant-mode strength claim."}
    report["candidate_speed_percent"] = round(
        (report["median_nps"]["candidate"] / report["median_nps"]["baseline"] - 1) * 100, 2)
    encoded = json.dumps(report, indent=2) + "\n"
    print(encoded, end="")
    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(encoded)
    if len(nodes) != 1:
        raise SystemExit("Normal-search signature differs; investigate before calling this a speed optimization")


if __name__ == "__main__":
    main()
