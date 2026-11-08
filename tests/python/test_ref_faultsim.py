"""ref_faultsim.py on c17, and the faultline CLI against it.

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
CLI = os.environ.get("FAULTLINE_CLI", str(ROOT / "build" / "debug" / "faultline"))
sys.path.insert(0, str(ROOT / "tools"))
import faultsim_check  # noqa: E402
import ref_faultsim  # noqa: E402

# All 32 vectors in counting order; inputs 1 2 3 6 7.
EXHAUSTIVE = [[int(bit) for bit in format(v, "05b")] for v in range(32)]


class RefFaultsimC17(unittest.TestCase):
    def test_all_stem_faults_detected(self):
        results = ref_faultsim.fault_simulate(str(C17), EXHAUSTIVE)
        self.assertEqual(len(results), 22)  # 11 nets x {SA0, SA1}
        self.assertTrue(all(first is not None for first in results.values()))

    def test_hand_checked_first_vectors(self):
        # Same cases as the C++ test C17HandCheckedDetections.
        results = ref_faultsim.fault_simulate(str(C17), EXHAUSTIVE)
        self.assertEqual(results[("10", 1)], 20)  # needs 1 = 3 = 1 and 2 = 0: 10100
        self.assertEqual(results[("22", 0)], 8)   # first vector with 22 = 1: 01000

    def test_one_vector(self):
        # 10100 gives outputs 22 = 1, 23 = 0.
        results = ref_faultsim.fault_simulate(str(C17), [[1, 0, 1, 0, 0]])
        self.assertIsNone(results[("22", 1)])
        self.assertEqual(results[("22", 0)], 0)
        self.assertIsNone(results[("23", 0)])
        self.assertEqual(results[("23", 1)], 0)


class FaultsimCliVsOracle(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.vectors = Path(self.tmp.name) / "vectors.txt"
        self.vectors.write_text("".join(format(v, "05b") + "\n" for v in range(32)))

    def run_cli(self, *args):
        result = subprocess.run([CLI, *args], capture_output=True, text=True, check=False)
        self.assertEqual(result.returncode, 0, result.stderr)
        return result.stdout

    def test_c17_same_detections(self):
        out = self.run_cli("faultsim", str(C17), "--vectors", str(self.vectors), "--detections",
                           "--undetected")
        self.assertIn("coverage:   100.00%", out)
        self.assertEqual(faultsim_check.parse_cli_output(out),
                         ref_faultsim.fault_simulate(str(C17), EXHAUSTIVE))

    def test_bitparallel_sim_matches_scalar_sim(self):
        scalar = self.run_cli("sim", str(C17), str(self.vectors))
        parallel = self.run_cli("sim", str(C17), str(self.vectors), "--bitparallel")
        self.assertEqual(len(scalar.split()), 32)
        self.assertEqual(scalar, parallel)


if __name__ == "__main__":
    unittest.main()
