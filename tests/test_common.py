import unittest

from mxlperf.common import compact_cpus, expand_cpu_spec


class CommonTests(unittest.TestCase):
    def test_cpu_specs(self):
        self.assertEqual(expand_cpu_spec("0-2,5,7-8"), [0, 1, 2, 5, 7, 8])
        self.assertEqual(compact_cpus([0, 1, 2, 5, 7, 8]), "0-2,5,7-8")


if __name__ == "__main__":
    unittest.main()
