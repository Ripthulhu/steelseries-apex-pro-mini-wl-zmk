"""Cross-check compiled ARM packet code against Python AESCCM and HMAC.

Run with --workspace pointing to the pinned ZMK workspace. Requires unicorn,
pyelftools and cryptography. No keyboard or dongle is accessed.
"""
import argparse
import hmac
from pathlib import Path
import struct
import subprocess
import unittest

from cryptography.hazmat.primitives.ciphers.aead import AESCCM
from elftools.elf.elffile import ELFFile
from unicorn import Uc, UC_ARCH_ARM, UC_MODE_THUMB, UC_MODE_MCLASS, UC_HOOK_CODE
from unicorn.arm_const import UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2, UC_ARM_REG_R3, UC_ARM_REG_SP, UC_ARM_REG_LR

KEY = bytes(range(16))
KN = bytes(range(16, 32))
DN = bytes(range(32, 48))
DERIVED = hmac.digest(KEY, b'apex-radio-v1-session' + KN + DN, 'sha256')
CTX, PEER, IN, OUT, TYPE, DATA = 0x20001000, 0x20002000, 0x20003000, 0x20004000, 0x20005000, 0x20006000


class PacketTests(unittest.TestCase):
    def setUp(self):
        self.cpu = Uc(UC_ARCH_ARM, UC_MODE_THUMB | UC_MODE_MCLASS)
        self.cpu.mem_map(0, 0x80000)
        self.cpu.mem_map(0x20000000, 0x20000)
        for start, data in SEGMENTS:
            self.cpu.mem_write(start, data)
        self.cpu.hook_add(UC_HOOK_CODE, self.stop)
        self.cpu.mem_write(IN, KEY + KN + DN)
        self.assertEqual(self.call('apex_session_init', CTX, IN, IN + 16, IN + 32, 0), 0)
        self.assertEqual(self.call('apex_session_init', PEER, IN, IN + 16, IN + 32, 1), 0)

    def stop(self, cpu, address, size, user):
        if address == 0x100:
            self.returned = True
            cpu.emu_stop()

    def call(self, name, *args):
        self.returned = False
        for reg, value in zip((UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2, UC_ARM_REG_R3), args):
            self.cpu.reg_write(reg, value)
        self.cpu.reg_write(UC_ARM_REG_SP, 0x2001e000)
        self.cpu.reg_write(UC_ARM_REG_LR, 0x101)
        if len(args) > 4:
            self.cpu.mem_write(0x2001e000, struct.pack('<' + 'I' * (len(args) - 4), *args[4:]))
        self.cpu.emu_start(SYMBOLS[name] | 1, 0, timeout=2_000_000, count=1000000)
        self.assertTrue(self.returned)
        return struct.unpack('<i', struct.pack('<I', self.cpu.reg_read(UC_ARM_REG_R0)))[0]

    def encode(self, payload, sender=CTX):
        self.cpu.mem_write(DATA, payload or b'\x00')
        n = self.call('apex_packet_encode', sender, 1, DATA, len(payload), OUT, 88)
        self.assertEqual(n, 24 + len(payload))
        return bytes(self.cpu.mem_read(OUT, n))

    def decode(self, packet, receiver=PEER):
        self.cpu.mem_write(IN, packet)
        self.cpu.mem_write(DATA, b'\xa5' * 64)
        self.cpu.mem_write(TYPE, b'\xa5')
        return self.call('apex_packet_decode', receiver, IN, len(packet), TYPE, DATA, 64)

    def test_reference_vectors(self):
        for n in (0, 1, 8, 32, 64):
            with self.subTest(length=n):
                data = bytes(range(n))
                wire = self.encode(data)
                nonce = wire[8:16] + wire[4:8] + wire[2:3]
                self.assertEqual(wire[8:16], DERIVED[16:24])
                self.assertEqual(wire[16:], AESCCM(DERIVED[:16], tag_length=8).encrypt(nonce, data, wire[:16]))
                self.assertEqual(self.decode(wire), n)
                self.assertEqual(bytes(self.cpu.mem_read(DATA, n)), data)

    def test_bidirectional_and_reflection(self):
        wire = self.encode(b'downlink', PEER)
        self.assertEqual(self.decode(wire, CTX), 8)
        self.assertEqual(self.decode(wire, PEER), -1)

    def test_tamper_does_not_consume_counter_or_modify_output(self):
        wire = self.encode(b'keys')
        size = self.call('fixture_session_size')
        before = bytes(self.cpu.mem_read(PEER, size))
        for i in range(len(wire)):
            corrupted = bytearray(wire)
            corrupted[i] ^= 1
            self.assertLess(self.decode(bytes(corrupted)), 0)
            self.assertEqual(bytes(self.cpu.mem_read(DATA, 64)), b'\xa5' * 64)
            self.assertEqual(bytes(self.cpu.mem_read(TYPE, 1)), b'\xa5')
            self.assertEqual(bytes(self.cpu.mem_read(PEER, size)), before)
        self.assertEqual(self.decode(wire), 4)

    def test_duplicates_out_of_order_and_old_packets(self):
        packets = [self.encode(bytes([i])) for i in range(66)]
        self.assertEqual(self.decode(packets[65]), 1)
        self.assertEqual(self.decode(packets[65]), -3)
        self.assertEqual(self.decode(packets[2]), 1)
        self.assertEqual(self.decode(packets[2]), -3)
        self.assertEqual(self.decode(packets[1]), -3)

    def test_truncated_and_oversized(self):
        wire = self.encode(bytes(64))
        for n in range(len(wire)):
            self.assertEqual(self.decode(wire[:n] or b'\x00'), -1)
        self.assertEqual(self.decode(wire + b'\x00'), -1)
        self.assertEqual(self.call('apex_packet_encode', CTX, 1, DATA, 65, OUT, 100), -1)

    def test_counter_exhaustion_and_clear(self):
        self.call('fixture_exhaust_counter', CTX)
        self.assertEqual(self.call('apex_packet_encode', CTX, 1, DATA, 1, OUT, 88), -4)
        self.call('apex_session_clear', CTX)
        size = self.call('fixture_session_size')
        self.assertEqual(bytes(self.cpu.mem_read(CTX, size)), bytes(size))
        self.assertEqual(self.call('apex_packet_encode', CTX, 1, DATA, 1, OUT, 88), -1)

    def test_new_session_rejects_old_traffic(self):
        wire = self.encode(b'old session')
        self.cpu.mem_write(IN, KEY + KN + bytes(range(48, 64)))
        self.assertEqual(self.call('apex_session_init', PEER, IN, IN + 16, IN + 32, 1), 0)
        self.assertEqual(self.decode(wire), -1)

    def test_invalid_role_disables_session(self):
        self.cpu.mem_write(IN, KEY + KN + DN)
        self.assertEqual(self.call('apex_session_init', CTX, IN, IN + 16, IN + 32, 2), -1)
        self.assertEqual(self.call('apex_packet_encode', CTX, 1, DATA, 1, OUT, 88), -1)

    def input_queue(self):
        self.cpu.mem_write(CTX, bytes(1024))
        self.call('apex_input_session', CTX)
        self.assertEqual(self.call('apex_input_ack', CTX, 1), 1)
        self.assertEqual(self.call('apex_input_ack', CTX, 2), 1)

    def input_push(self, body, kind=1):
        self.cpu.mem_write(DATA, body)
        return self.call('apex_input_push', CTX, kind, DATA, len(body))

    def input_head(self):
        self.assertEqual(self.call('apex_input_peek', CTX, OUT), 1)
        n = self.call('apex_input_pack', OUT, IN, 64)
        self.assertGreater(n, 0)
        return bytes(self.cpu.mem_read(IN, n))

    def test_input_short_tap_and_lost_ack(self):
        self.input_queue()
        press = bytes([2, 0, 4, 0, 0, 0, 0, 0])
        self.assertEqual(self.input_push(press), 0)
        self.assertEqual(self.input_push(bytes(8)), 0)
        expected = bytes([1]) + struct.pack('<I', 3) + bytes([1]) + press
        self.assertEqual(self.input_head(), expected)
        self.assertEqual(self.call('apex_input_ack', CTX, 0), 0)
        self.assertEqual(self.call('apex_input_ack', CTX, 2), 0)
        self.assertEqual(self.input_head(), expected)
        self.assertEqual(self.call('apex_input_ack', CTX, 4), 0)
        self.assertEqual(self.input_head(), expected)
        self.assertEqual(self.call('apex_input_ack', CTX, 3), 1)
        self.assertEqual(self.input_head()[6:], bytes(8))
        self.assertEqual(self.call('apex_input_ack', CTX, 3), 0)
        self.assertEqual(self.call('apex_input_ack', CTX, 4), 1)
        self.assertEqual(self.call('apex_input_peek', CTX, OUT), 0)

    def test_input_disconnect_discards_old_taps(self):
        self.input_queue()
        self.input_push(bytes([0, 0, 4, 0, 0, 0, 0, 0]))
        self.input_push(bytes(8))
        self.call('apex_input_disconnect', CTX)
        held = bytes([0, 0, 5, 0, 0, 0, 0, 0])
        self.input_push(held)
        self.assertEqual(self.call('apex_input_peek', CTX, OUT), 0)
        self.call('apex_input_session', CTX)
        self.assertEqual(self.input_head()[6:], held)
        self.assertEqual(self.call('apex_input_ack', CTX, 1), 1)
        self.assertEqual(self.input_head()[5:], bytes([2]) + bytes(12))

    def test_input_full_queue_keeps_latest_state(self):
        self.input_queue()
        for i in range(32):
            self.assertEqual(self.input_push(bytes([0, 0, i + 4, 0, 0, 0, 0, 0])), 0)
        self.assertEqual(self.input_push(bytes(8)), -2)
        self.call('apex_input_session', CTX)
        self.assertEqual(self.input_head()[6:], bytes(8))

    def test_input_format_validation(self):
        self.input_queue()
        self.assertEqual(self.input_push(bytes(7)), -1)
        self.assertEqual(self.input_push(bytes(8), 9), -1)
        self.input_push(bytes([1]) + bytes(11), 2)
        wire = self.input_head()
        self.assertEqual(self.call('apex_input_unpack', IN, len(wire), OUT), 0)
        for bad in (wire[:-1], wire + b'\x00', b'\x02' + wire[1:],
                    wire[:1] + bytes(4) + wire[5:], wire[:5] + b'\x09' + wire[6:]):
            self.cpu.mem_write(IN, bad)
            self.assertEqual(self.call('apex_input_unpack', IN, len(bad), OUT), -1)

    def connections(self):
        self.cpu.mem_write(IN, KEY + KN + DN + b'pair-id!')
        for ctx, nonce, role in [(CTX, IN + 16, 0), (PEER, IN + 32, 1)]:
            self.assertEqual(self.call('apex_connection_init', ctx, IN, IN + 48, role), 0)
            self.assertEqual(self.call('apex_connection_start', ctx, nonce), 0)

    def handshake_request(self, ctx):
        n = self.call('apex_connection_request', ctx, OUT, 88)
        self.assertEqual(n, 58)
        return bytes(self.cpu.mem_read(OUT, n))

    def handshake_receive(self, ctx, packet):
        self.cpu.mem_write(IN, packet)
        n = self.call('apex_connection_receive', ctx, IN, len(packet), OUT, 88)
        return n, bytes(self.cpu.mem_read(OUT, n)) if n > 0 else b''

    def handshake(self):
        self.connections()
        hello = self.handshake_request(CTX)
        self.assertEqual(hello[42:], hmac.digest(KEY, b'apex-radio-v1-handshake' + hello[:42], 'sha256')[:16])
        n, challenge = self.handshake_receive(PEER, hello)
        self.assertEqual(n, 58)
        n, confirm = self.handshake_receive(CTX, challenge)
        self.assertEqual(n, 58)
        n, ready = self.handshake_receive(PEER, confirm)
        self.assertEqual(n, 58)
        self.assertEqual(self.handshake_receive(CTX, ready)[0], 0)
        return hello, challenge, confirm, ready

    def test_handshake_then_data(self):
        self.connections()
        self.assertEqual(self.call('apex_connection_encode', CTX, 1, DATA, 1, OUT, 88), -5)
        self.handshake()
        self.cpu.mem_write(DATA, b'key')
        n = self.call('apex_connection_encode', CTX, 1, DATA, 3, OUT, 88)
        self.assertEqual(n, 27)
        self.assertEqual(self.call('apex_connection_decode', PEER, OUT, n, TYPE, DATA, 64), 3)
        self.assertEqual(bytes(self.cpu.mem_read(DATA, 3)), b'key')

    def test_handshake_retransmissions_never_reset_session(self):
        hello, challenge, confirm, ready = self.handshake()
        self.assertEqual(self.call('apex_connection_encode', CTX, 1, DATA, 1, OUT, 88), 25)
        size = self.call('fixture_connection_size')
        before_k = bytes(self.cpu.mem_read(CTX, size))
        before_d = bytes(self.cpu.mem_read(PEER, size))
        self.assertEqual(self.handshake_receive(PEER, hello)[0], -5)
        self.assertEqual(self.handshake_receive(CTX, challenge)[0], -5)
        self.assertEqual(self.handshake_receive(PEER, confirm)[1], ready)
        self.assertEqual(self.handshake_receive(CTX, ready)[0], 0)
        self.assertEqual(bytes(self.cpu.mem_read(CTX, size)), before_k)
        self.assertEqual(bytes(self.cpu.mem_read(PEER, size)), before_d)

    def test_handshake_tamper_and_retries(self):
        self.connections()
        hello = self.handshake_request(CTX)
        size = self.call('fixture_connection_size')
        before = bytes(self.cpu.mem_read(PEER, size))
        for i in range(len(hello)):
            bad = bytearray(hello); bad[i] ^= 1
            self.assertLess(self.handshake_receive(PEER, bytes(bad))[0], 0)
            self.assertEqual(bytes(self.cpu.mem_read(PEER, size)), before)
        _, challenge = self.handshake_receive(PEER, hello)
        self.assertEqual(self.handshake_receive(PEER, hello)[1], challenge)
        _, confirm = self.handshake_receive(CTX, challenge)
        self.assertEqual(self.handshake_receive(CTX, challenge)[1], confirm)
        self.assertEqual(self.handshake_request(CTX), confirm)

    def test_stale_handshake_and_local_nonce_reuse(self):
        hello, challenge, confirm, ready = self.handshake()
        self.cpu.mem_write(IN, KN)
        self.assertEqual(self.call('apex_connection_start', CTX, IN), -1)
        self.cpu.mem_write(IN, bytes(range(64, 80)))
        self.assertEqual(self.call('apex_connection_start', CTX, IN), 0)
        self.assertEqual(self.handshake_receive(CTX, challenge)[0], -5)
        self.assertEqual(self.handshake_receive(CTX, ready)[0], -5)
        self.cpu.mem_write(IN, bytes(range(80, 96)))
        self.assertEqual(self.call('apex_connection_start', PEER, IN), 0)
        self.assertEqual(self.handshake_receive(PEER, confirm)[0], -5)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--workspace', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    radio = Path(__file__).resolve().parents[1]
    tiny = args.workspace.resolve() / 'modules/crypto/tinycrypt/lib'
    sdk = args.workspace.resolve() / '.zephyr-sdk/arm-zephyr-eabi/bin'
    gcc = next(sdk.glob('arm-zephyr-eabi-gcc*'))
    args.output.mkdir(parents=True, exist_ok=True)
    binary = args.output.resolve() / 'packet-test.elf'
    command = [str(gcc), '-mcpu=cortex-m4', '-mthumb', '-Os', '-g', '-nostartfiles',
               '-Wl,-Ttext=0x1000', '-Wl,-e,apex_session_init',
               '-I' + str(radio / 'include'), '-I' + str(tiny / 'include'),
               str(radio / 'src/apex_packet.c'), str(radio / 'src/apex_connection.c'),
               str(radio / 'src/apex_input.c'),
               str(radio / 'tests/packet_fixture.c')]
    command += [str(tiny / 'source' / n) for n in ('aes_encrypt.c', 'ccm_mode.c', 'hmac.c', 'sha256.c', 'utils.c')]
    subprocess.run(command + ['-o', str(binary)], check=True)
    with binary.open('rb') as f:
        elf = ELFFile(f)
        SYMBOLS = {s.name: int(s['st_value']) for s in elf.get_section_by_name('.symtab').iter_symbols()}
        SEGMENTS = [(int(s['p_vaddr']), s.data()) for s in elf.iter_segments()
                    if s['p_type'] == 'PT_LOAD' and s['p_filesz']]
    unittest.main(argv=['packet-tests'], verbosity=2)
