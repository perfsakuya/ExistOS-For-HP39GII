"""Synthetic session boundaries and metric formats for the serial dashboard."""

from pathlib import Path
from tempfile import TemporaryDirectory
import unittest

import serial_dashboard


class DashboardStateTest(unittest.TestCase):
    def setUp(self):
        self.temp = TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.log = Path(self.temp.name) / "serial.log"
        self.previous_log = serial_dashboard.LOG
        self.previous_follow = serial_dashboard.FOLLOW_DIR
        serial_dashboard.LOG = self.log
        serial_dashboard.FOLLOW_DIR = None
        self.addCleanup(self.restore)

    def restore(self):
        serial_dashboard.LOG = self.previous_log
        serial_dashboard.FOLLOW_DIR = self.previous_follow

    def read(self, content):
        self.log.write_text(content, encoding="ascii")
        return serial_dashboard.state()

    def test_hybrid_metrics_replace_old_modes(self):
        s = self.read(
            "DOOM_MODE flat_walls=0\nDOOM_START allocated=1\n"
            "DOOM_PERF frames=16 wall_ms=16000 convert_ms=16 display_ms=16 other_ms=15968\n"
            "DOOMLITE_PERF frames=32 elapsed_ms=1060 render_ms=300 queue_ms=400 max_interval_ms=47 map=0\n"
            "DOOMLITE_EXIT frames=32\n"
            "DOOM_MODE flat_walls=0 fast=1\nDOOM_START allocated=1\nDOOM_ZONE verified bytes=262144\n"
            "DOOM_FAST frames=32 elapsed_ms=1100 render_ms=256 present_ms=384 "
            "max_interval_ms=48 max_render_ms=12 max_present_ms=15 hp=87 kills=3 blue=1 map=0\n"
            "DOOM_TIC tics=35 sum_ms=70 max_ms=4\n"
            "DOOM_PHASE frames=16 logic_ms=96 draw_ms=400 logic_max_ms=9 draw_max_ms=27\n"
        )
        self.assertEqual(s["mode"], "hybrid")
        self.assertEqual(s["stage"], "E1M1 混合版运行中")
        self.assertIsNone(s["performance"])
        self.assertIsNone(s["lite"])
        self.assertIsNone(s["render"])
        self.assertEqual(s["fast"]["fps"], 29.09)
        self.assertEqual(s["fast"]["frame_ms"], 34.4)
        self.assertEqual(s["fast"]["max_interval_ms"], 48)
        self.assertEqual(s["fast"]["render_ms"], 8.0)
        self.assertEqual(s["fast"]["max_render_ms"], 12)
        self.assertEqual(s["fast"]["present_ms"], 12.0)
        self.assertEqual(s["fast"]["max_present_ms"], 15)
        self.assertEqual(s["fast"]["health"], 87)
        self.assertTrue(s["fast"]["blue_key"])
        self.assertEqual(s["tic"], {"average_ms": 2.0, "max_ms": 4})
        self.assertEqual(s["phases"]["logic_max_ms"], 9)
        self.assertEqual(s["phases"]["draw_max_ms"], 27)

    def test_new_start_clears_previous_session_metrics(self):
        s = self.read(
            "DOOM_MODE fast=1\nDOOM_START allocated=1\n"
            "DOOM_FAST frames=32 elapsed_ms=1060 render_ms=250 present_ms=400 "
            "max_interval_ms=40\nDOOM_TIC tics=35 sum_ms=70 max_ms=4\n"
            "DOOM_MODE flat_walls=0 fast=0\nDOOM_START allocated=2\n"
        )
        self.assertEqual(s["mode"], "legacy")
        self.assertIsNone(s["fast"])
        self.assertIsNone(s["tic"])
        self.assertIsNone(s["phases"])
        self.assertIsNone(s["performance"])
        self.assertIsNone(s["lite"])

    def test_legacy_and_lite_logs_remain_supported(self):
        old = (
            "DOOM_MODE flat_walls=0\nDOOM_START allocated=1\nDOOM_ZONE verified bytes=262144\n"
            "DOOM_PERF frames=16 wall_ms=16000 convert_ms=32 display_ms=192 other_ms=15776\n"
            "DOOM_PHASE frames=16 logic_ms=7040 draw_ms=9600\n"
            "DOOM_RENDER frames=16 setup_ms=16 bsp_ms=8000 planes_ms=1600 masked_ms=0\n"
        )
        s = self.read(old)
        self.assertEqual(s["mode"], "legacy")
        self.assertEqual(s["performance"]["fps"], 1.0)
        self.assertEqual(s["phases"]["logic_ms"], 440.0)
        self.assertIsNone(s["phases"]["logic_max_ms"])
        self.assertEqual(s["render"]["bsp_ms"], 500)
        self.assertIsNone(s["fast"])
        s = self.read(old +
            "DOOMLITE_PERF frames=32 elapsed_ms=1060 render_ms=300 "
            "queue_ms=400 max_interval_ms=47 map=1\n")
        self.assertEqual(s["mode"], "lite")
        self.assertIsNone(s["performance"])
        self.assertIsNone(s["phases"])
        self.assertIsNone(s["render"])
        self.assertTrue(s["lite"]["map"])

    def test_optional_fast_maxima_and_post_start_mode(self):
        s = self.read(
            "DOOM_START allocated=1\nDOOM_MODE fast=1\n"
            "DOOM_FAST frames=32 elapsed_ms=1060 render_ms=256 present_ms=384 "
            "max_interval_ms=45 hp=100 kills=0 blue=0 map=1\n"
        )
        self.assertEqual(s["mode"], "hybrid")
        self.assertIsNone(s["fast"]["max_render_ms"])
        self.assertIsNone(s["fast"]["max_present_ms"])
        self.assertFalse(s["fast"]["blue_key"])
        self.assertTrue(s["fast"]["map"])

    def test_mode_before_start_allows_intervening_logs_without_inheritance(self):
        previous = (
            "DOOM_MODE fast=1\nDOOM_START allocated=1\n"
            "DOOM_FAST frames=32 elapsed_ms=1000 render_ms=240 present_ms=360 "
            "max_interval_ms=40\nDOOM_EXIT outcome=0\n"
            "SYSTEM STATUS UI resumed\n"
        )
        s = self.read(previous + "DOOM_START allocated=2\n")
        self.assertEqual(s["mode"], "legacy")
        self.assertIsNone(s["fast"])
        s = self.read(previous +
            "DOOM_MODE flat_walls=0 fast=1\n"
            "UI waiting for key release\nSYSTEM STATUS heartbeat\n"
            "DOOM_START allocated=3\nDOOM_ZONE verified bytes=262144\n"
            "DOOM_FAST frames=32 elapsed_ms=1050 render_ms=250 present_ms=400 "
            "max_interval_ms=46 max_render_ms=11 max_present_ms=16\n")
        self.assertEqual(s["mode"], "hybrid")
        self.assertEqual(s["fast"]["max_interval_ms"], 46)


if __name__ == "__main__":
    unittest.main()
