import json
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
DASHBOARD_DIR = ROOT / "monitoring" / "dashboards"
EXPECTED_DASHBOARDS = {
    "fps-grafana-dashboard.json": "MXL FFmpeg FPS",
    "pcm-extended-grafana-dashboard.json": "Intel PCM — Extended Hardware Counters (Deep Dive)",
    "pcm-grafana-dashboard.json": "Intel PCM — k8s-w2 (FFmpeg-MXL Scenario 1)",
    "perfspect-grafana-dashboard.json": "Intel PerfSpect — k8s-w2 (FFmpeg-MXL Scenario 1)",
    "system-overview-grafana-dashboard.json": "CRM — System Overview",
}
PROMETHEUS_DS = {"type": "prometheus", "uid": "prometheus"}
MONITORING_README = ROOT / "monitoring" / "README.md"
OBSERVABILITY_DOC = ROOT / "docs" / "06-observability.md"


def iter_panels(panels):
    for panel in panels:
        yield panel
        yield from iter_panels(panel.get("panels", []))


class MonitoringDashboardTests(unittest.TestCase):
    def load_dashboard(self, name):
        return json.loads((DASHBOARD_DIR / name).read_text())

    def test_all_expected_dashboards_exist_and_are_valid_json(self):
        self.assertEqual(
            {path.name for path in DASHBOARD_DIR.glob("*.json")},
            set(EXPECTED_DASHBOARDS),
        )

        seen_uids = set()
        for name, expected_title in EXPECTED_DASHBOARDS.items():
            with self.subTest(dashboard=name):
                dashboard = self.load_dashboard(name)
                self.assertEqual(dashboard["title"], expected_title)
                self.assertIsInstance(dashboard["schemaVersion"], int)
                self.assertTrue(dashboard["panels"])
                self.assertIn("uid", dashboard)
                self.assertNotIn(dashboard["uid"], seen_uids)
                seen_uids.add(dashboard["uid"])
                self.assertIsNone(
                    dashboard.get("id"),
                    "Grafana dashboard imports should omit the top-level id or set it to null.",
                )

                panel_ids = [panel["id"] for panel in iter_panels(dashboard["panels"]) if "id" in panel]
                self.assertEqual(len(panel_ids), len(set(panel_ids)))

    def test_dashboards_only_use_expected_datasource_shapes(self):
        for name in EXPECTED_DASHBOARDS:
            with self.subTest(dashboard=name):
                dashboard = self.load_dashboard(name)
                for panel in iter_panels(dashboard["panels"]):
                    datasource = panel.get("datasource", "missing")
                    self.assertIn(datasource, ("missing", None, PROMETHEUS_DS))
                for variable in dashboard.get("templating", {}).get("list", []):
                    datasource = variable.get("datasource", "missing")
                    self.assertIn(datasource, ("missing", PROMETHEUS_DS))
                for annotation in dashboard.get("annotations", {}).get("list", []):
                    datasource = annotation.get("datasource", "missing")
                    self.assertIn(
                        datasource,
                        (
                            "missing",
                            None,
                            PROMETHEUS_DS,
                            {"type": "grafana", "uid": "-- Grafana --"},
                        ),
                    )

    def test_pcm_dashboards_keep_safe_promql_conventions(self):
        ratio_panels_by_dashboard = {
            "pcm-grafana-dashboard.json": {
                "L3 (LLC) hit ratio (%)",
                "Average active core frequency (GHz)",
                "Avg active freq now (GHz)",
            },
            "pcm-extended-grafana-dashboard.json": {
                "Avg active freq (GHz)",
                "L3 hit ratio (%)",
                "IPC — Instructions per Cycle (system)",
                "L3 hit ratio (%) — system",
                "L2 hit ratio (%) — system",
                "L3 miss rate (%) — system",
                "Average active core frequency (GHz)",
                "Active frequency per socket (GHz)",
            },
        }

        for name, ratio_panels in ratio_panels_by_dashboard.items():
            with self.subTest(dashboard=name):
                dashboard = self.load_dashboard(name)
                title_to_exprs = {
                    panel["title"]: [target["expr"] for target in panel.get("targets", []) if "expr" in target]
                    for panel in iter_panels(dashboard["panels"])
                    if panel.get("targets")
                }

                for title in ratio_panels:
                    self.assertIn(title, title_to_exprs)
                    for expr in title_to_exprs[title]:
                        self.assertIn("clamp_min(", expr)

                for exprs in title_to_exprs.values():
                    for expr in exprs:
                        if 'aggregate="system"' in expr:
                            self.assertIn('socket=""', expr)
                        if 'socket=~"$socket"' in expr:
                            self.assertIn('aggregate=""', expr)

    def test_known_dashboard_units_are_importable(self):
        fps_dashboard = self.load_dashboard("fps-grafana-dashboard.json")
        fps_units = {
            panel["title"]: panel["fieldConfig"]["defaults"].get("unit")
            for panel in iter_panels(fps_dashboard["panels"])
            if "fieldConfig" in panel
        }
        self.assertEqual(fps_units["Frame Throughput (rate frame_total)"], "fps")

        perfspect_dashboard = self.load_dashboard("perfspect-grafana-dashboard.json")
        perfspect_units = {
            panel["title"]: panel["fieldConfig"]["defaults"].get("unit")
            for panel in iter_panels(perfspect_dashboard["panels"])
            if "fieldConfig" in panel
        }
        self.assertEqual(perfspect_units["Frequency — core vs uncore (GHz)"], "ghz")

    def test_dashboards_include_new_cpu_and_pcm_panels(self):
        fps_titles = {
            panel.get("title")
            for panel in iter_panels(self.load_dashboard("fps-grafana-dashboard.json")["panels"])
        }
        fps_variables = {
            variable.get("name")
            for variable in self.load_dashboard("fps-grafana-dashboard.json").get("templating", {}).get("list", [])
        }
        self.assertTrue(
            {
                "CPU Utilisation & Saturation",
                "Host CPU utilisation (%) — node-exporter",
                "Workload CPU consumption (cores) — cAdvisor",
            }.issubset(fps_titles)
        )
        self.assertIn("namespace", fps_variables)

        pcm_titles = {
            panel.get("title")
            for panel in iter_panels(self.load_dashboard("pcm-extended-grafana-dashboard.json")["panels"])
        }
        self.assertTrue(
            {
                "L2 hit ratio (%) — system",
                "L3 miss rate (%) — system",
                "Interconnect / NUMA",
                "UPI incoming utilisation — max and per socket (%)",
                "UPI incoming utilisation per link (%)",
                "UPI incoming utilisation vs DRAM bandwidth",
                "Reference clock vs active cycles — system total (cycles/s)",
            }.issubset(pcm_titles)
        )

    def test_monitoring_docs_link_dashboard_import_guidance(self):
        monitoring_readme = MONITORING_README.read_text()
        self.assertIn("copy/paste", monitoring_readme)
        self.assertIn("dashboards/pcm-extended-grafana-dashboard.json", monitoring_readme)

        observability_doc = OBSERVABILITY_DOC.read_text()
        self.assertIn("../monitoring/README.md", observability_doc)
        self.assertIn("../monitoring/dashboards/", observability_doc)


if __name__ == "__main__":
    unittest.main()
