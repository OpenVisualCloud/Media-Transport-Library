import unittest

from mxlperf.cli import power_mismatches, wait_for_pcm_cross_numa


class CliTests(unittest.TestCase):
    def test_pinned_pcm_preflight_accepts_all_required_series(self):
        queries = []

        def query_fn(_url, query):
            queries.append(query)
            return [{"value": [102, "3"]}]

        wait_for_pcm_cross_numa(
            "http://prometheus",
            'job="pcm-sensor-server",instance=~"10.0.0.9(:.*)?"',
            query_fn=query_fn,
        )
        self.assertEqual(len(queries), 1)
        self.assertTrue(all(
            'instance=~"10.0.0.9(:.*)?"' in query
            and "time() - max(timestamp(label_replace(" in query
            and '"pcm_link","$1","__name__","(.*)"' in query
            for query in queries
        ))

    def test_pinned_pcm_preflight_rejects_missing_exporter(self):
        with self.assertRaisesRegex(RuntimeError, "requires pcm-sensor-server"):
            wait_for_pcm_cross_numa("http://prometheus", "")

    def test_pinned_pcm_preflight_reports_empty_query(self):
        with self.assertRaisesRegex(RuntimeError, "UPI: query returned no series"):
            wait_for_pcm_cross_numa(
                "http://prometheus",
                'job="pcm-sensor-server"',
                timeout=0,
                query_fn=lambda _url, _query: [],
            )

    def test_pinned_pcm_preflight_reports_stale_sample(self):
        with self.assertRaisesRegex(RuntimeError, "UPI: newest sample is 31.0s old"):
            wait_for_pcm_cross_numa(
                "http://prometheus",
                'job="pcm-sensor-server"',
                timeout=0,
                query_fn=lambda _url, _query: [{"value": [102, "31"]}],
            )

    def test_pinned_pcm_preflight_reports_clock_skew(self):
        with self.assertRaisesRegex(RuntimeError, "sample is 5.0s in the future"):
            wait_for_pcm_cross_numa(
                "http://prometheus",
                'job="pcm-sensor-server"',
                timeout=0,
                query_fn=lambda _url, _query: [{"value": [102, "-5"]}],
            )


class PowerMismatchTests(unittest.TestCase):
    CONFIG = {
        "LAB_POWER_GOVERNOR": "performance",
        "LAB_POWER_PSTATE_DRIVER": "active",
        "LAB_POWER_EPB": "0",
        "LAB_POWER_EPP": "0",
    }

    def spec(self, **overrides):
        power = {
            "scaling_driver": "intel_pstate",
            "pstate_status": "active",
            "no_turbo": "0",
            "governor": "performance",
            "epb": "0",
            "epp": "performance",
        }
        power.update(overrides)
        return {"power": power}

    def test_configured_platform_reports_nothing(self):
        self.assertEqual(power_mismatches(self.CONFIG, self.spec()), [])

    def test_epp_name_is_never_compared_against_the_number(self):
        # sysfs reports 'performance' while LAB_POWER_EPP is 0. Comparing those
        # would make every correctly configured node look misconfigured.
        self.assertEqual(power_mismatches(self.CONFIG, self.spec(epp="balance_power")), [])

    def test_wrong_governor_is_reported(self):
        problems = power_mismatches(self.CONFIG, self.spec(governor="powersave"))
        self.assertEqual(len(problems), 1)
        self.assertIn("LAB_POWER_GOVERNOR=performance", problems[0])
        self.assertIn("powersave", problems[0])

    def test_every_wrong_setting_is_reported(self):
        problems = power_mismatches(
            self.CONFIG,
            self.spec(governor="performance,powersave", pstate_status="passive", epb="6"),
        )
        self.assertEqual(len(problems), 3)

    def test_unexposed_setting_is_not_a_mismatch(self):
        self.assertEqual(power_mismatches(self.CONFIG, self.spec(epb="", pstate_status="")), [])

    def test_skip_silences_the_comparison(self):
        config = dict(self.CONFIG, LAB_POWER_PSTATE_DRIVER="skip")
        self.assertEqual(power_mismatches(config, self.spec(pstate_status="passive")), [])

    def test_probe_without_a_power_section_is_safe(self):
        self.assertEqual(power_mismatches(self.CONFIG, {}), [])


if __name__ == "__main__":
    unittest.main()