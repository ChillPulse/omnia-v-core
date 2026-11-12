#!/usr/bin/env python3
"""Стендовый UART-инструмент AT32. Только с отключённым усилителем и ESP32 UART.
Требуется pyserial; физический переходник UART только 3,3 В.
"""
import argparse
import binascii
import struct
import sys
import time

SYNC = b'\x55\xaa'
MAX_PAYLOAD = 544


def frame(seq, command, payload=b''):
    body = struct.pack('<HBBB', len(payload), 0x21, seq, command) + payload
    return SYNC + body + struct.pack('<H', binascii.crc_hqx(body, 0xffff))


class Parser:
    def __init__(self):
        self.buffer = bytearray()

    def feed(self, data):
        self.buffer.extend(data)
        result = []
        while True:
            pos = self.buffer.find(SYNC)
            if pos < 0:
                self.buffer[:] = self.buffer[-1:] if self.buffer.endswith(SYNC[:1]) else b''
                break
            del self.buffer[:pos]
            if len(self.buffer) < 7:
                break
            size = int.from_bytes(self.buffer[2:4], 'little')
            if size > MAX_PAYLOAD:
                del self.buffer[0]
                continue
            total = 9 + size
            if len(self.buffer) < total:
                break
            candidate = self.buffer[:total]
            expected = int.from_bytes(candidate[-2:], 'little')
            if binascii.crc_hqx(candidate[2:-2], 0xffff) != expected:
                del self.buffer[0]
                continue
            result.append((candidate[5], candidate[6], bytes(candidate[7:-2])))
            del self.buffer[:total]
        return result


class Link:
    def __init__(self, serial):
        self.serial, self.parser, self.seq = serial, Parser(), 0

    def send(self, command, payload=b''):
        self.seq = (self.seq + 1) & 255
        self.serial.write(frame(self.seq, command, payload))
        return self.seq

    def wait(self, predicate, timeout=2.0):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            for msg in self.parser.feed(self.serial.read(256)):
                if predicate(*msg):
                    return msg
        raise TimeoutError('Нет ожидаемого ответа AT32. Проверьте питание, SRAM, TX/RX и скорость.')

    def command(self, cmd, payload=b''):
        seq = self.send(cmd, payload)
        _, _, data = self.wait(lambda s, c, p: s == seq and c == 0x82 and len(p) >= 2 and p[0] == cmd)
        if data[1] != 0:
            raise RuntimeError('AT32 отклонил команду 0x%02X: статус %u' % (cmd, data[1]))

    def status(self):
        return self.wait(lambda s, c, p: c == 0x80 and len(p) >= 25, 4.0)[2]

    def info(self):
        seq = self.send(0x33)
        return self.wait(lambda s, c, p: s == seq and c == 0x85 and len(p) == 18)[2]


def main():
    parser = argparse.ArgumentParser(description='Статус и тестовый тон AT32 без усилителя')
    parser.add_argument('--port', required=True, help='COM3 / /dev/ttyUSB0 — UART-адаптер 3,3 В')
    parser.add_argument('--tone', type=int, default=0, metavar='СЕК', help='Тон 1 кГц, от 1 до 30 секунд')
    parser.add_argument('--no-amp', action='store_true', help='Подтверждаю: K2, усилитель и динамики физически отключены')
    args = parser.parse_args()
    if args.tone < 0 or args.tone > 30 or (args.tone and not args.no_amp):
        parser.error('--tone 1..30 требует --no-amp и физически отключённый усилитель')
    try:
        import serial
    except ImportError:
        parser.error('Установите зависимость: python -m pip install pyserial')
    try:
        with serial.Serial(args.port, baudrate=921600, timeout=0.1, write_timeout=1) as port:
            link = Link(port)
            major, minor, patch, ext, ram, hz, jedec, flash = struct.unpack('<BBBBHIII', link.info())
            print('AT32 FW %d.%d.%d; SRAM %d KiB; SYSCLK %d Hz; SPI id 0x%06x' %
                  (major, minor, patch, ram, hz, jedec))
            st = link.status()
            print('Fs %d Hz; mute=%d; источник=%d; DC=%d; K2=%d; PB4 mode=%d' %
                  (int.from_bytes(st[:4], 'little'), st[6], st[8], st[13], st[23], st[24]))
            if not args.tone:
                return
            if st[13] or st[23]:
                raise RuntimeError('Обнаружена DC-авария или включён K2; тест запрещён')
            print('Включаем тестовый тон; не подключать усилитель или динамики!')
            # Always attempt hard mute on interruption or error after starting.
            try:
                link.command(0x01, struct.pack('<HB', 25, 1))
                link.command(0x03, struct.pack('<fffH', -24.0, 0.0, 0.0, 40))
                link.command(0x05, struct.pack('<Bff', 3, 1000.0, -24.0))
                link.command(0x02, struct.pack('<HB', 35, 0))
                time.sleep(args.tone)
            finally:
                try:
                    link.command(0x01, struct.pack('<HB', 25, 1))
                    print('Тон остановлен, XSMT выключен.')
                except Exception as exc:
                    print('Не удалось подтвердить MUTE: отключите питание! ' + str(exc), file=sys.stderr)
    except (OSError, TimeoutError, RuntimeError, serial.SerialException) as exc:
        print('Ошибка: ' + str(exc), file=sys.stderr)
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
