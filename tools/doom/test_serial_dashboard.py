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

    def test_compact_game_metrics_and_exit_are_session_scoped(self):
        previous = (
            "DOOM_MODE fast=1\nDOOM_START allocated=1\n"
            "DOOM_FAST frames=32 elapsed_ms=1100 render_ms=320 present_ms=384 "
            "max_interval_ms=41\n"
            "DOOMG_BOOT phase=task_start ms=1000\n"
            "DOOMG_BOOT phase=ready ms=1020\n"
            "DOOMG_PERF frames=32 elapsed_ms=1064 logic_ticks=35 "
            "logic_total_ms=70 logic_max_ms=4 logic_total_us=70350 logic_max_us=3600 "
            "render_total_ms=320 render_max_ms=14 render_total_us=320000 render_max_us=14100 "
            "lcd_total_ms=384 lcd_max_ms=16 lcd_total_us=384000 lcd_max_us=15900 "
            "interval_samples=31 interval_total_ms=1031 interval_max_ms=44 dropped_ticks=0 "
            "hp=92 ammo=48 kills=1 blue=1 map=0 x=-416 y=256\n"
        )
        s = self.read(previous)
        self.assertEqual(s["mode"], "game")
        self.assertEqual(s["stage"], "E1M1 Game 运行中")
        self.assertIsNone(s["fast"])
        self.assertEqual(s["game"]["fps"], 30.08)
        self.assertEqual(s["game"]["logic_ms"], 2.01)
        self.assertEqual(s["game"]["logic_max_ms"], 3.6)
        self.assertEqual(s["game"]["render_ms"], 10)
        self.assertIsNone(s["game"]["scene_ms"])
        profiled = self.read(previous.replace("x=-416 y=256",
            "x=-416 y=256 zoom=2 scene_total_us=256000 scene_max_us=11000 "
            "sprite_total_us=32000 sprite_max_us=2200 hud_total_us=32000 hud_max_us=1200 "
            "weapon_total_us=16000 weapon_max_us=900 pistol_tics=15"))
        self.assertEqual(profiled["game"]["scene_ms"], 8)
        self.assertEqual(profiled["game"]["sprite_ms"], 1)
        self.assertEqual(profiled["game"]["hud_ms"], 1)
        self.assertEqual(profiled["game"]["sprite_max_ms"], 2.2)
        self.assertEqual(profiled["game"]["zoom"], 2)
        self.assertEqual(profiled["game"]["weapon_ms"], 0.5)
        self.assertEqual(profiled["game"]["weapon_max_ms"], 0.9)
        self.assertEqual(profiled["game"]["pistol_tics"], 15)
        self.assertEqual(s["game"]["lcd_ms"], 12)
        self.assertEqual(s["game"]["interval_max_ms"], 44)
        self.assertEqual(s["game"]["interval_ms"], 33.26)
        self.assertTrue(s["game"]["blue_key"])
        s = self.read(previous + "DOOMG_EXIT phase=ui_resume_begin ms=2999\n")
        self.assertEqual(s["stage"], "E1M1 Game 退出处理中")
        s = self.read(previous + "DOOMG_EXIT phase=ui_resume_done ms=3000\n")
        self.assertEqual(s["stage"], "E1M1 Game 已退出")
        s = self.read(previous + "DOOMG_EXIT phase=ui_resume_done ms=3000\n"
                      "DOOMLITE_PERF frames=32 elapsed_ms=1060 render_ms=300 "
                      "queue_ms=400 max_interval_ms=47 map=0\n")
        self.assertEqual(s["mode"], "lite")
        self.assertIsNone(s["game"])

    def test_game_memory_replaces_stale_system_sample_and_preserves_units(self):
        idle = (
            "Allocate MEM:57/276 KB\nZRAM:24/92 KB\n"
            "SRAM Heap Pre-allocated: 58 KB\nSwap Heap Pre-allocated: 0 KB\n"
        )
        s = self.read(idle)
        self.assertEqual(s["memory_kb"], (57, 276))
        self.assertEqual(s["heap_preallocated_kb"], (58, 0))

        game = idle + "DOOMG_BOOT phase=task_start ms=1000\n"
        s = self.read(game)
        self.assertEqual(s["mode"], "game")
        self.assertIsNone(s["memory_kb"])
        self.assertIsNone(s["zram_kb"])
        self.assertEqual(s["memory_history"], [])

        game += "DOOMG_MEM a=61440/282624 z=25088/94208 s=65536 w=0\r\n"
        game += "DOOMG_MEM a=62464/282624 z=26112/94208 s=71680 w=1024\n"
        s = self.read(game)
        self.assertEqual(s["memory_kb"], (61.0, 276.0))
        self.assertEqual(s["zram_kb"], (25.5, 92.0))
        self.assertEqual(s["heap_preallocated_kb"], (70.0, 1.0))
        self.assertEqual(s["memory_history"][-2:], [60.0, 61.0])

        exited = game + "DOOMG_EXIT phase=ui_resume_done ms=3000\n"
        s = self.read(exited)
        self.assertIsNone(s["memory_kb"])
        self.assertIsNone(s["zram_kb"])
        self.assertIsNone(s["heap_preallocated_kb"])

        after = (exited + "Allocate MEM:57/276 KB\nZRAM:27/92 KB\n"
                 "SRAM Heap Pre-allocated: 70 KB\n"
                 "Swap Heap Pre-allocated: 1 KB\n")
        s = self.read(after)
        self.assertEqual(s["memory_kb"], (57, 276))
        self.assertEqual(s["zram_kb"], (27, 92))
        self.assertEqual(s["heap_preallocated_kb"], (70, 1))
        s = self.read(after + "DOOMG_BOOT phase=task_start ms=4000\n")
        self.assertIsNone(s["memory_kb"])

    def test_two_levels_details_and_packet_boundaries(self):
        boot = ("DOOMG_BOOT phase=task_start ms=1000\n"
                "DOOMG_BOOT phase=game_init state_bytes=7220 render_bytes=9381 world_bytes=218116 sprite_bytes=129221\n")
        perf = (
            "DOOMG_PERF frames=32 elapsed_ms=1600 logic_ticks=56 "
            "logic_total_ms=112 logic_max_ms=4 render_total_ms=800 render_max_ms=30 "
            "lcd_total_ms=384 lcd_max_ms=16 interval_samples=31 interval_total_ms=1550 "
            "interval_max_ms=60 dropped_ticks=0 hp=150 ammo=90 kills=2 blue=1 "
            "red=1 yellow=0 armor=100 contrast=3 level=2 actors=93 movers=1 frame_count=96\n"
        )
        detail = (
            "DOOMG_DETAIL frames=32 tics=56 level=2 ai_total_us=56000 ai_max_us=2300 "
            "anim_total_us=11200 anim_max_us=260 mover_total_us=2800 mover_max_us=200 "
            "collision_total_us=5600 collision_max_us=300 los_total_us=8400 los_max_us=700 "
            "cells=128000 line_tests=96000 actor_peak=93 mover_peak=1 overflows=0 "
            "ray_total_us=320000 ray_max_us=13000 plane_total_us=96000 plane_max_us=4500 "
            "wall_total_us=160000 wall_max_us=6600 world_pixels=256000 plane_pixels=64000 "
            "span_total_bytes=262144 span_peak_bytes=12288\n"
        )
        s = self.read(boot + perf + detail)
        self.assertEqual(s["stage"], "E1M2 Game 运行中")
        self.assertEqual(s["game"]["level"], 2)
        self.assertEqual(s["starts"], 1)
        self.assertEqual(s["game_resources"], {"state_kb": 7.05, "render_kb": 9.16})
        self.assertEqual(s["game_world_kb"], 213.0)
        self.assertEqual(s["game_sprite_kb"], 126.19)
        self.assertIsNone(self.read(boot.replace(" sprite_bytes=129221", "") + perf)["game_sprite_kb"])
        self.assertEqual(s["latest_frame"], 96)
        self.assertEqual(s["game"]["armor"], 100)
        self.assertTrue(s["game"]["red_key"])
        self.assertFalse(s["game"]["yellow_key"])
        self.assertEqual(s["game"]["contrast"], 3)
        self.assertEqual(s["game_detail"]["ai_ms"], 1)
        self.assertEqual(s["game_detail"]["ai_max_ms"], 2.3)
        self.assertEqual(s["game_detail"]["los_ms"], 0.15)
        self.assertEqual(s["game_detail"]["raw"]["actor_peak"], 93)
        self.assertEqual(s["game_detail"]["ray_ms"], 10)
        self.assertEqual(s["game_detail"]["plane_ms"], 3)
        self.assertEqual(s["game_detail"]["wall_ms"], 5)
        self.assertEqual(s["game_detail"]["ray_max_ms"], 13)
        self.assertEqual(s["game_detail"]["span_active_kb"], 8)
        self.assertEqual(s["game_detail"]["span_peak_kb"], 12)
        legacy_detail = detail.replace(" span_total_bytes=262144 span_peak_bytes=12288", "")
        legacy = self.read(boot + perf + legacy_detail)
        self.assertIsNone(legacy["game_detail"]["span_active_kb"])
        self.assertIsNone(legacy["game_detail"]["span_peak_kb"])
        s = self.read(boot + perf + detail + perf)
        self.assertIsNone(s["game_detail"])  # Do not reuse a prior batch's timing.
        s = self.read(boot + perf + detail + "DOOMG_BOOT phase=map_load level=1 duration_us=150\n")
        self.assertEqual(s["stage"], "E1M1 Game 启动中")
        self.assertIsNone(s["game"])
        self.assertIsNone(s["game_detail"])
        s = self.read(boot + perf + detail + boot)
        self.assertIsNone(s["game_detail"])
        self.assertEqual(s["starts"], 2)
        s = self.read(boot + perf + detail + "DOOMG_EXIT phase=key frames=96\n"
                      "DOOMG_EXIT phase=ui_resume_done\nDOOMG_EXIT phase=task_delete\n")
        self.assertEqual(s["exits"], 1)


if __name__ == "__main__":
    unittest.main()
