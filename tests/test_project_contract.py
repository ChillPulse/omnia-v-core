"""Small consistency checks for the published configuration, not a board test."""
import pathlib
import re
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]


class ProjectContract(unittest.TestCase):
    def test_partitions_contiguous_and_spec_matches(self):
        entries = []
        for line in (ROOT / 'partitions.csv').read_text().splitlines():
            if not line.strip() or line.lstrip().startswith('#'):
                continue
            name, typ, subtype, offset, size, *_ = [x.strip() for x in line.split(',')]
            entries.append((name, typ, subtype, int(offset, 0), int(size, 0)))
        self.assertEqual(len(entries), 6)
        for a, b in zip(entries, entries[1:]):
            self.assertLessEqual(a[3] + a[4], b[3], f'{a[0]} overlaps {b[0]}')
        self.assertLessEqual(entries[-1][3] + entries[-1][4], 16 * 1024 * 1024)
        spec = (ROOT / 'docs/OMNIA_MASTER_SPEC_v2.0.md').read_text()
        for name, typ, subtype, offset, size in entries:
            row = next((line for line in spec.splitlines() if line.startswith(f'| {name} |')), None)
            self.assertIsNotNone(row, name)
            self.assertIn(f'{typ}/{subtype}', row)
            self.assertIn(f'0x{offset:06x}', row.lower())
            self.assertIn(f'0x{size:x}', row.lower())

    def test_declared_esp_pin_assignments_do_not_overlap(self):
        text = (ROOT / 'shared/omnia_pins.h').read_text()
        matches = re.findall(r'^#define\s+(OMNIA_ESP_\w+_GPIO)\s+(\d+)\b', text, re.M)
        pins = {}
        for name, raw in matches:
            pin = int(raw)
            self.assertNotIn(pin, pins, f'{name} conflicts with {pins.get(pin)}')
            self.assertFalse(26 <= pin <= 37, f'{name} conflicts with flash/PSRAM')
            pins[pin] = name
        for pin in (0, 1, 2, 3, 4, 5, 6, 7, 8, 19, 20, 40, 45, 46, 48):
            self.assertIn(pin, pins)

    def test_selected_stlink_upload_profile(self):
        text = (ROOT / 'platformio.ini').read_text()
        at32 = text.split('[env:at32f403_dsp]', 1)[1]
        self.assertRegex(at32, r'(?m)^upload_protocol\s*=\s*stlink\s*$')

    def test_unverified_automatic_sample_rate_switch_disabled(self):
        text = (ROOT / 'src/esp32/at32_bridge.c').read_text()
        body = text.split('esp_err_t omnia_at32_switch_sample_rate_atomic(', 1)[1].split('\n}', 1)[0]
        self.assertIn('return ESP_ERR_NOT_SUPPORTED;', body)
        self.assertNotIn('OMNIA_CMD_UNMUTE', body)
        self.assertNotIn('OMNIA_CMD_SET_FS', body)

    def test_legacy_and_deferred_default_off(self):
        text = (ROOT / 'src/at32/include/omnia_at32.h').read_text()
        for feature in ('BT', 'USB_DAC'):
            self.assertRegex(text, rf'#define\s+OMNIA_ENABLE_{feature}\s+0\b')
        storage = (ROOT / 'src/at32/spi_flash_sfdp.c').read_text()
        self.assertIn('#if 0', storage)
        self.assertIn('quarantined legacy implementation', storage)


if __name__ == '__main__':
    unittest.main()
