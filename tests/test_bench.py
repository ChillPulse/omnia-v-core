import importlib.util
import pathlib
import unittest

path = pathlib.Path(__file__).resolve().parents[1] / 'scripts' / 'omnia_bench.py'
spec = importlib.util.spec_from_file_location('omnia_bench', path)
b = importlib.util.module_from_spec(spec)
spec.loader.exec_module(b)


class BenchTest(unittest.TestCase):
    def test_crc(self):
        self.assertEqual(b.binascii.crc_hqx(b'123456789', 0xffff), 0x29b1)

    def test_known_frame(self):
        self.assertEqual(b.frame(7, 0x24, b"\x01\x02").hex(), "55aa02002107240102902b")

    def test_fragmented_noise_crc(self):
        good = b.frame(7, 0x24, b'\x01\x02')
        damaged = bytearray(good)
        damaged[-1] ^= 2
        parser = b.Parser()
        parsed = []
        stream = b'noise' + bytes(damaged) + good
        for octet in stream:
            parsed.extend(parser.feed(bytes([octet])))
        self.assertEqual(parsed, [(7, 0x24, b'\x01\x02')])

    def test_multiple_packets(self):
        p = b.Parser()
        self.assertEqual(p.feed(b.frame(0, 0x34) + b.frame(1, 3, b'abc')),
                         [(0, 0x34, b''), (1, 3, b'abc')])

    def test_bogus_length(self):
        p = b.Parser()
        self.assertEqual(p.feed(b'\x55\xaa\xff\xff\x20\x01\x34' + b.frame(2, 0x34)),
                         [(2, 0x34, b'')])


if __name__ == '__main__':
    unittest.main()
