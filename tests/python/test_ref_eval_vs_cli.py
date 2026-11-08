"""ref_eval.py and the faultline CLI must agree on c17 for all 32 vectors.

Run by ctest, which sets FAULTLINE_CLI to the built binary. By hand:
    FAULTLINE_CLI=build/debug/faultline python3 -m unittest discover -s tests/python
"""

import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
C17 = ROOT / "benchmarks" / "iscas85" / "c17.bench"
REF_EVAL = ROOT / "tools" / "ref_eval.py"
CLI = os.environ.get("FAULTLINE_CLI", str(ROOT / "build" / "debug" / "faultline"))


def run(command):
    return subprocess.run(command, capture_output=True, text=True, check=False)


class RefEvalVsCli(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)

    def write(self, name, text):
        path = Path(self.tmp.name) / name
        path.write_text(text)
        return str(path)

    def test_c17_exhaustive(self):
        lines = ["# all 32 vectors, inputs 1 2 3 6 7"]
        lines += [format(v, "05b") for v in range(32)]
        vectors = self.write("vectors.txt", "\n".join(lines) + "\n")

        cli = run([CLI, "sim", str(C17), vectors])
        ref = run([sys.executable, str(REF_EVAL), str(C17), vectors])
        self.assertEqual(cli.returncode, 0, cli.stderr)
        self.assertEqual(ref.returncode, 0, ref.stderr)
        self.assertEqual(len(cli.stdout.split()), 32)
        self.assertEqual(cli.stdout, ref.stdout)

    def test_info(self):
        result = run([CLI, "info", str(C17)])
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("gates:   6", result.stdout)
        self.assertIn("depth:   3", result.stdout)

    def test_parse_error_has_file_and_line(self):
        bench = self.write("bad.bench", "INPUT(a)\nOUTPUT(y)\ny = AND(a, ghost)\n")
        vectors = self.write("v.txt", "1\n")
        result = run([CLI, "sim", bench, vectors])
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("bad.bench:3:", result.stderr)
        self.assertIn("undefined net 'ghost'", result.stderr)


if __name__ == "__main__":
    unittest.main()
