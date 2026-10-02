"""Offline checks for the generated Freedoom E1M1 gameplay table."""

import struct
import unittest

import build_lite_game_data as game


def lump_directory_entry(data: bytes, name: bytes) -> int:
    _, count, directory = struct.unpack_from("<4sII", data)
    for index in range(count):
        entry = directory + index * 16
        if data[entry + 8:entry + 16].rstrip(b"\0") == name:
            return entry
    raise AssertionError(f"missing lump {name!r}")


class E1M1GameplayDataTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.source = game.SOURCE.read_bytes()

    def test_header_is_reproducible(self):
        self.assertEqual(game.render_header(self.source),
                         game.OUTPUT.read_text(encoding="ascii"))

    def test_key_exit_and_closed_doors(self):
        things, lines, doors = game.extract(self.source)
        self.assertEqual((len(things), len(lines), len(doors)), (292, 42, 15))
        self.assertEqual([(thing.x, thing.y) for thing in things
                          if thing.type == 5], [(2192, 576)])
        exit_line, = (item for item in lines if item.line.special == 11)
        self.assertEqual(exit_line.line.index, 407)
        self.assertEqual(exit_line.back_sector, game.NO_SECTOR)
        self.assertEqual({door.sector for door in doors
                          if door.kind == game.DOOR_BLUE}, {51, 71})
        for door in doors:
            self.assertLess(door.x0, door.x1)
            self.assertLess(door.y0, door.y1)
            self.assertEqual(door.floor, door.closed_ceiling)
            self.assertGreater(door.open_ceiling, door.closed_ceiling)

    def test_rejects_lost_key(self):
        changed = bytearray(self.source)
        entry = lump_directory_entry(changed, b"THINGS")
        offset = struct.unpack_from("<I", changed, entry)[0]
        struct.pack_into("<h", changed, offset + 87 * game.THING_RECORD.size + 6,
                         6)
        with self.assertRaisesRegex(ValueError, "blue key"):
            game.extract(changed)

    def test_rejects_changed_exit(self):
        changed = bytearray(self.source)
        entry = lump_directory_entry(changed, b"LINEDEFS")
        offset = struct.unpack_from("<I", changed, entry)[0]
        struct.pack_into("<h", changed, offset + 407 * game.LINE_RECORD.size + 50,
                         0)
        with self.assertRaisesRegex(ValueError, "special LINEDEFS changed"):
            game.extract(changed)

    def test_rejects_nonrectangular_door(self):
        changed = bytearray(self.source)
        entry = lump_directory_entry(changed, b"LINEDEFS")
        offset = struct.unpack_from("<I", changed, entry)[0]
        line = offset + 55 * game.LINE_RECORD.size
        struct.pack_into("<i", changed, line, 900 << 16)
        struct.pack_into("<i", changed, line + 20, (768 - 900) << 16)
        with self.assertRaisesRegex(ValueError, "door sector 10: not an axis"):
            game.extract(changed)


if __name__ == "__main__":
    unittest.main()
