import unittest

from mxlperf.report import numa_bandwidth_rows, per_core_usage


class ReportTests(unittest.TestCase):
    def test_numa_bandwidth_is_split_by_socket(self):
        rows = [
            {"metric": "socket_local_memory_bandwidth_bytes_per_second", "value": "avg=8000000000;min=0;max=0", "scope": '{"socket":"0"}'},
            {"metric": "socket_remote_memory_bandwidth_bytes_per_second", "value": "avg=2000000000;min=0;max=0", "scope": '{"socket":"0"}'},
            {"metric": "socket_upi_incoming_bytes_per_second", "value": "avg=3000000000;min=0;max=0", "scope": '{"socket":"0"}'},
            {"metric": "socket_local_memory_bandwidth_bytes_per_second", "value": "avg=9000000000;min=0;max=0", "scope": '{"socket":"1"}'},
            {"metric": "socket_remote_memory_bandwidth_bytes_per_second", "value": "avg=1000000000;min=0;max=0", "scope": '{"socket":"1"}'},
        ]

        bandwidth = numa_bandwidth_rows(rows)

        self.assertEqual(bandwidth[0]["Local memory bandwidth (GB/s)"], 8.0)
        self.assertEqual(bandwidth[0]["Remote memory bandwidth (GB/s)"], 2.0)
        self.assertEqual(bandwidth[0]["Remote memory ratio (%)"], 20.0)
        self.assertEqual(bandwidth[0]["UPI incoming bandwidth (GB/s)"], 3.0)
        self.assertEqual(bandwidth[1]["Remote memory ratio (%)"], 10.0)

    def test_per_core_usage_aggregates_ffmpeg_roles(self):
        rows = [
            {"cpu_id": "10", "role": "encoder", "ffmpeg_cpu_seconds": "60"},
            {"cpu_id": "10", "role": "decoder", "ffmpeg_cpu_seconds": "12"},
            {"cpu_id": "12", "role": "encoder", "ffmpeg_cpu_seconds": "30"},
        ]

        usage = per_core_usage(rows, 120, [
            {"cpu_id": "10", "real_cpu_total_usage_pct": "76.5"},
            {"cpu_id": "12", "real_cpu_total_usage_pct": "31.25"},
        ])

        self.assertEqual(usage[0]["CPU ID"], 10)
        self.assertEqual(usage[0]["FFmpeg avg usage (%)"], 60.0)
        self.assertEqual(usage[0]["Encoder avg usage (%)"], 50.0)
        self.assertEqual(usage[0]["Real CPU total usage (%)"], 76.5)
        self.assertEqual(usage[0]["Decoder avg usage (%)"], 10.0)
        self.assertEqual(usage[1]["FFmpeg avg usage (%)"], 25.0)


if __name__ == "__main__":
    unittest.main()
