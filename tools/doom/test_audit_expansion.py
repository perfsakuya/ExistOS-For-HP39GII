"""Checks for boundaries that matter when auditing arbitrary standard IWADs."""
import struct
import unittest

from audit_expansion import RECORDS, directory, map_audit, post_payload_bytes


def small_map(sectors=1):
    lumps = {name: b"" for name in RECORDS}
    lumps["VERTEXES"] = struct.pack("<4h", 0, 0, 64, 64)
    lumps["SIDEDEFS"] = RECORDS["SIDEDEFS"].pack(0, 0, b"", b"", b"", 0)
    lumps["LINEDEFS"] = RECORDS["LINEDEFS"].pack(0, 1, 1, 26, 0, 0, 65535)
    sector = RECORDS["SECTORS"].pack(0, 128, b"", b"", 160, 0, 0)
    lumps["SECTORS"] = sector * sectors
    # Medium, easy-only and multiplayer-only instances of a supported enemy.
    lumps["THINGS"] = b"".join(RECORDS["THINGS"].pack(16, 16, 0, 3004, flags)
                                for flags in (2, 1, 18))
    lumps.update(REJECT=b"", BLOCKMAP=b"")
    return lumps


class AuditTests(unittest.TestCase):
    def test_truncated_directory_is_rejected(self):
        with self.assertRaisesRegex(ValueError, "directory"):
            directory(struct.pack("<4sII", b"IWAD", 1, 12))

    def test_lump_cannot_read_outside_wad(self):
        data = struct.pack("<4sII", b"IWAD", 1, 12)
        data += struct.pack("<II8s", 1000, 4, b"THINGS")
        with self.assertRaisesRegex(ValueError, "outside"):
            directory(data)

    def test_difficulty_filter_and_sector_sentinel(self):
        result = map_audit("TEST", small_map(sectors=255))
        self.assertEqual(result["medium_sp_monsters"], 1)
        self.assertFalse(result["current_sector_u8_sentinel_fits"])
        self.assertEqual(result["grid_32unit_dimensions"], [5, 5])

    def test_invalid_sidedef_is_rejected(self):
        data = small_map()
        data["LINEDEFS"] = RECORDS["LINEDEFS"].pack(0, 1, 1, 0, 0, 1, 65535)
        with self.assertRaisesRegex(ValueError, "invalid side"):
            map_audit("TEST", data)

    def test_column_runs_include_transparent_gaps(self):
        # Two separate one-pixel runs need their own top/length headers.
        self.assertEqual(post_payload_bytes(1, 4, [1, None, 2, None]), 9)
        self.assertEqual(post_payload_bytes(1, 4, [None] * 4), 3)


if __name__ == "__main__":
    unittest.main()
