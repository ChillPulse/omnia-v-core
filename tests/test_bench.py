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
        self.assertEqual(parsed, [(7, 0x24, 1, b'\x01\x02')])

    def test_multiple_packets(self):
        p = b.Parser()
        self.assertEqual(p.feed(b.frame(0, 0x34) + b.frame(1, 3, b'abc')),
                         [(0, 0x34, 1, b''), (1, 3, 1, b'abc')])

    def test_bogus_length(self):
        p = b.Parser()
        self.assertEqual(p.feed(b'\x55\xaa\xff\xff\x20\x01\x34' + b.frame(2, 0x34)),
                         [(2, 0x34, 1, b'')])

    @staticmethod
    def event(seq, cmd, flags, payload=b'', version=2):
        body = b.struct.pack('<HBBB', len(payload), (version << 4) | flags, seq, cmd) + payload
        return b.SYNC + body + b.struct.pack('<H', b.binascii.crc_hqx(body, 0xffff))

    class FakeSerial:
        def __init__(self, reads):
            self.reads = list(reads)
            self.writes = []
        def write(self, data):
            self.writes.append(data)
            return len(data)
        def read(self, size):
            return self.reads.pop(0) if self.reads else b''

    def test_info_and_status_in_one_read(self):
        info, status = bytes(18), bytes(25)
        dev = self.FakeSerial([self.event(1, 0x85, 2, info) + self.event(7, 0x80, 0, status)])
        link = b.Link(dev)
        self.assertEqual(link.info(), info)
        self.assertEqual(link.status(), status)  # tail of the previous serial read was retained

    def test_ack_requires_flags_and_success_status(self):
        ack = self.event(1, 0x82, 2, b'\x01\x00')
        link = b.Link(self.FakeSerial([self.event(1, 0x82, 0, b'\x01\x00') + ack]))
        link.command(1)
        for flags, status in ((4, 0), (4, 2), (2, 2)):
            link = b.Link(self.FakeSerial([self.event(1, 0x82, flags, bytes((1, status)))]))
            with self.assertRaises(RuntimeError):
                link.command(1)

    def test_wrong_version_is_rejected(self):
        p = b.Parser()
        self.assertEqual(p.feed(self.event(5, 0x80, 0, b'', version=3) + self.event(6, 0x80, 0)),
                         [(6, 0x80, 0, b'')])

    def test_stop_attempts_both_and_reports_failure(self):
        class FakeLink:
            def __init__(self, fail):
                self.calls, self.fail = [], fail
            def command(self, cmd, payload):
                self.calls.append(cmd)
                if cmd == self.fail:
                    raise TimeoutError('no ack')
        for fail in (1, 5):
            link = FakeLink(fail)
            with self.assertRaisesRegex(RuntimeError, 'отключите питание'):
                b.stop_tone(link)
            self.assertEqual(link.calls, [1, 5])


if __name__ == '__main__':
    unittest.main()
