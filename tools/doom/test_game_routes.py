import unittest

import audit_game_routes as routes
import build_game_maps as maps


class GameRouteTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        entries = maps.directory(maps.SOURCE.read_bytes())
        cls.maps = [maps.extract(name, i, maps.map_lumps(entries, name))
                    for i, name in enumerate(("E1M1", "E1M2"))]

    def test_keys_and_exit_pass_necessary_conditions(self):
        for data in self.maps:
            report = routes.route_audit(data)
            self.assertTrue(report["all_key_pickups_reachable_optimistic"])
            self.assertTrue(report["all_exit_front_sectors_reachable"])
            self.assertTrue(all(record["native_supported"] or record["cosmetic_only"]
                                for record in report["special_records"]))

    def test_red_key_requires_original_pickup_height(self):
        data = self.maps[1]
        key = data.things[104]
        self.assertEqual(key[:2], (672, -2960))
        self.assertEqual(key[5], 99)
        self.assertEqual(data.sectors[99][0] - data.sectors[141][0], 32)
        floors = [{sector[0]} for sector in data.sectors]
        reach = {141}
        self.assertIsNone(routes.pickup_reach(data, key, reach, floors, max_above=24))
        self.assertIsNotNone(routes.pickup_reach(data, key, reach, floors))

    def test_mandatory_floor_target_and_full_width_sector(self):
        data = self.maps[1]
        yellow_line = data.lines[1904]
        self.assertEqual((yellow_line[5], yellow_line[6]), (71, 34))
        self.assertEqual(routes.targets(data, yellow_line), [301])
        self.assertEqual(routes.target_height(data.sectors[301], 71), -40)
        high_line = data.lines[846]
        self.assertEqual(routes.targets(data, high_line), [379])
        self.assertEqual(routes.target_height(data.sectors[379], 19), -24)

    def test_action_target_refs_exist_and_floor_minimum_matches_doom(self):
        for data in self.maps:
            for sector in data.sectors:
                self.assertLessEqual(sector[5], sector[0])
            for line_index in data.special_refs:
                line = data.lines[line_index]
                if line[5] not in (11, 48, 97):
                    self.assertTrue(routes.targets(data, line), (data.name, line_index))
                if line[5] == 97:
                    self.assertTrue(any(thing[3] == 14 and data.sectors[thing[5]][4] == line[6]
                                        for thing in data.things))

    def test_mover_target_does_not_invent_body_clearance(self):
        # Empty decoration can legitimately close to less than 56 units;
        # an occupied mover pauses rather than choosing a different height.
        sector = (0, 32, 160, 0, 1, 0, 8, 8, 32, 32, 0, 0, 0, 0, 32, 32)
        self.assertEqual(routes.target_height(sector, 58), 24)
        self.assertEqual(routes.target_height(sector, 19), 8)


if __name__ == "__main__":
    unittest.main()
