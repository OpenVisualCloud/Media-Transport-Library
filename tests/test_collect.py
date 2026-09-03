import unittest

from mxlperf.collect import aggregate_metric, upi_incoming_query


class CollectTests(unittest.TestCase):
    def test_upi_query_sums_available_links_without_vector_intersection(self):
        query = upi_incoming_query('job="pcm-sensor-server",pod=~"pcm-worker"')
        self.assertIn('Incoming_Data_Traffic_On_Link_[0-3]', query)
        self.assertIn('pod=~"pcm-worker"', query)
        self.assertNotIn(" + ", query)

    def test_upi_counter_is_converted_to_exact_window_rate(self):
        series = [{"metric": {}, "values": [[100, "1000"], [110, "3100"]]}]
        result = aggregate_metric("cross_numa_upi_incoming_bytes_per_second", series)
        self.assertEqual(result, [{"labels": {}, "avg": 210.0, "min": 210.0, "max": 210.0}])

    def test_socket_upi_counter_keeps_socket_label(self):
        series = [{"metric": {"socket": "1"}, "values": [[20, "500"], [24, "900"]]}]
        result = aggregate_metric("socket_upi_incoming_bytes_per_second", series)
        self.assertEqual(result[0]["labels"], {"socket": "1"})
        self.assertEqual(result[0]["avg"], 100.0)


if __name__ == "__main__":
    unittest.main()