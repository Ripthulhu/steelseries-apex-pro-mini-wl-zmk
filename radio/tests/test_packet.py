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
HOP_K, HOP_D = 0x20007000, 0x20008000
LINK_K, LINK_D = 0x20009000, 0x2000a000


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

    def encode(self, payload, sender=CTX, packet_type=1):
        self.cpu.mem_write(DATA, payload or b'\x00')
        n = self.call('apex_packet_encode', sender, packet_type, DATA, len(payload), OUT, 88)
        self.assertEqual(n, 24 + len(payload))
        return bytes(self.cpu.mem_read(OUT, n))

    def decode(self, packet, receiver=PEER):
        self.cpu.mem_write(IN, packet)
        self.cpu.mem_write(DATA, b'\xa5' * 64)
        self.cpu.mem_write(TYPE, b'\xa5')
        return self.call('apex_packet_decode', receiver, IN, len(packet), TYPE, DATA, 64)

    def hop_init(self):
        self.handshake()
        self.assertEqual(self.call('apex_hop_init', HOP_K, CTX), 0)
        self.assertEqual(self.call('apex_hop_init', HOP_D, PEER), 0)

    def hop_offer(self, channels=(6, 26, 50, 74), generation=1):
        self.cpu.mem_write(DATA, bytes(channels))
        n = self.call('apex_hop_offer_map', HOP_K, CTX, DATA, len(channels), generation, OUT, 88)
        return n, bytes(self.cpu.mem_read(OUT, n)) if n > 0 else b''

    def hop_accept(self, packet):
        self.cpu.mem_write(IN, packet)
        return self.call('apex_hop_accept_map', HOP_D, PEER, IN, len(packet))

    def hop_sync(self, now):
        n = self.call('fixture_hop_sync', HOP_K, CTX, now, OUT, 88)
        self.assertEqual(n, 36)
        return bytes(self.cpu.mem_read(OUT, n))

    def hop_receive(self, packet, now):
        self.cpu.mem_write(IN, packet)
        return self.call('fixture_hop_receive', HOP_D, PEER, IN, len(packet), now)

    def test_hop_map_authentication_and_replay(self):
        self.hop_init()
        n, wire = self.hop_offer()
        self.assertEqual(n, 34)
        size = self.call('fixture_hop_size')
        before = bytes(self.cpu.mem_read(HOP_D, size))
        for i in range(n):
            bad = bytearray(wire); bad[i] ^= 1
            self.assertLess(self.hop_accept(bytes(bad)), 0)
            self.assertEqual(bytes(self.cpu.mem_read(HOP_D, size)), before)
        self.assertEqual(self.hop_accept(wire), 0)
        self.assertEqual(self.hop_accept(wire), -3)

    def test_hop_map_validation_and_ordering(self):
        self.hop_init()
        for channels in [(2, 4, 6), (2, 4, 4, 8), (2, 4, 5, 8),
                         (6, 4, 8, 10), (0, 4, 6, 8), (2, 4, 6, 81), tuple(range(2, 36, 2))]:
            self.assertEqual(self.hop_offer(channels)[0], -1)
        self.assertEqual(self.hop_offer(generation=0)[0], -1)
        _, old = self.hop_offer()
        _, new = self.hop_offer((8, 28, 52, 76), 2)
        self.assertEqual(self.hop_accept(new), 0)
        size = self.call('fixture_hop_size')
        before = bytes(self.cpu.mem_read(HOP_D, size))
        self.assertEqual(self.hop_accept(old), -1)
        self.assertEqual(bytes(self.cpu.mem_read(HOP_D, size)), before)
        self.assertEqual(self.hop_offer((8, 28, 52, 78), 2)[0], -1)
        _, retry = self.hop_offer((8, 28, 52, 76), 2)
        self.assertEqual(self.hop_accept(retry), 0)

    def test_hop_clock_survives_lost_packets(self):
        self.hop_init()
        self.assertEqual(self.hop_accept(self.hop_offer()[1]), 0)
        self.assertEqual(self.call('fixture_hop_begin', HOP_K, 1000, 0, 0), 0)
        self.assertEqual(self.hop_receive(self.hop_sync(1000), 9000), 0)
        # Independent local clocks with an 8 ms origin offset. No ACK advances
        # the schedule; losing four whole slots still yields the same channel.
        visited = []
        for elapsed in (0, 19999, 20000, 39999, 40000, 60000, 80000, 99999):
            k = self.call('fixture_hop_channel', HOP_K, 1000 + elapsed, 0)
            d = self.call('fixture_hop_channel', HOP_D, 9000 + elapsed, 0)
            self.assertEqual(k, d)
            visited.append(k)
        self.assertEqual(set(visited), {6, 26, 50, 74})
        self.assertEqual(self.call('fixture_hop_channel', HOP_D, 109000, 0), -6)
        self.assertEqual(self.hop_receive(self.hop_sync(101000), 109000), -6)

    def test_hop_every_map_size_visits_every_channel(self):
        for count in range(4, 17):
            self.hop_init()
            channels = tuple(range(2, count * 2 + 1, 2))
            self.assertEqual(self.hop_accept(self.hop_offer(channels)[1]), 0)
            self.assertEqual(self.call('fixture_hop_begin', HOP_K, 0, 0, 0), 0)
            seen = set()
            for slot in range(count * 2):
                now = slot * 20000
                self.assertEqual(self.hop_receive(self.hop_sync(now), now), 0)
                k = self.call('fixture_hop_channel', HOP_K, now, 0)
                self.assertEqual(k, self.call('fixture_hop_channel', HOP_D, now, 0))
                seen.add(k)
            self.assertEqual(seen, set(channels))

    def test_hop_late_sync_cannot_rewind_clock(self):
        self.hop_init()
        self.assertEqual(self.hop_accept(self.hop_offer()[1]), 0)
        self.assertEqual(self.call('fixture_hop_begin', HOP_K, 0, 0, 0), 0)
        old, new = self.hop_sync(1000), self.hop_sync(21000)
        self.assertEqual(self.hop_receive(new, 21000), 0)
        size = self.call('fixture_hop_size')
        before = bytes(self.cpu.mem_read(HOP_D, size))
        self.assertEqual(self.hop_receive(old, 22000), -1)
        self.assertEqual(bytes(self.cpu.mem_read(HOP_D, size)), before)
        self.assertEqual(self.hop_receive(new, 22000), -3)
        self.assertEqual(self.hop_offer(generation=2)[0], -5)

    def test_hop_clock_bounds_and_session_binding(self):
        self.hop_init()
        self.assertEqual(self.hop_accept(self.hop_offer()[1]), 0)
        self.assertEqual(self.call('fixture_hop_begin', HOP_D, 0, 0, 0), -5)
        self.assertEqual(self.call('fixture_hop_begin', HOP_K, 0xfffffff0, 1, 0xffffffff), 0)
        self.assertGreaterEqual(self.call('fixture_hop_channel', HOP_K, 0x4e0f, 2), 0)
        self.assertEqual(self.call('fixture_hop_channel', HOP_K, 0x4e10, 2), -4)
        self.assertEqual(self.call('fixture_hop_channel', HOP_K, 0xffffffff, 0xffffffff), -4)
        self.cpu.mem_write(IN, bytes(range(64, 80)))
        self.assertEqual(self.call('apex_connection_start', CTX, IN), 0)
        self.assertEqual(self.hop_offer(generation=2)[0], -5)

    def test_hop_sync_authentication_and_semantics(self):
        self.hop_init()
        self.assertEqual(self.hop_accept(self.hop_offer()[1]), 0)
        self.assertEqual(self.call('fixture_hop_begin', HOP_K, 0, 0, 0), 0)
        wire = self.hop_sync(1000)
        size = self.call('fixture_hop_size')
        before = bytes(self.cpu.mem_read(HOP_D, size))
        for i in range(len(wire)):
            bad = bytearray(wire); bad[i] ^= 1
            self.assertLess(self.hop_receive(bytes(bad), 1000), 0)
            self.assertEqual(bytes(self.cpu.mem_read(HOP_D, size)), before)
        self.assertEqual(self.hop_receive(wire, 1000), 0)
        before = bytes(self.cpu.mem_read(HOP_D, size))
        for generation, phase in [(2, 1000), (1, 20000), (1, 65535)]:
            payload = struct.pack('<BBIIH', 0x48, 1, generation, 0, phase)
            self.cpu.mem_write(DATA, payload)
            n = self.call('apex_connection_encode', CTX, 3, DATA, len(payload), OUT, 88)
            self.assertEqual(self.hop_receive(bytes(self.cpu.mem_read(OUT, n)), 2000), -1)
            self.assertEqual(bytes(self.cpu.mem_read(HOP_D, size)), before)
        # A fresh counter does not make a badly delayed clock sample current.
        self.assertEqual(self.hop_receive(self.hop_sync(2000), 5001), -1)
        self.assertEqual(bytes(self.cpu.mem_read(HOP_D, size)), before)
        self.assertEqual(self.hop_receive(self.hop_sync(6000), 6000), 0)

    def test_hop_old_session_schedule_is_not_reused(self):
        self.hop_init()
        old = self.hop_offer()[1]
        self.assertEqual(self.hop_accept(old), 0)
        for ctx, nonce in [(CTX, bytes(range(64, 80))), (PEER, bytes(range(80, 96)))]:
            self.cpu.mem_write(IN, nonce)
            self.assertEqual(self.call('apex_connection_start', ctx, IN), 0)
        _, challenge = self.handshake_receive(PEER, self.handshake_request(CTX))
        _, confirm = self.handshake_receive(CTX, challenge)
        _, ready = self.handshake_receive(PEER, confirm)
        self.assertEqual(self.handshake_receive(CTX, ready)[0], 0)
        self.assertEqual(self.hop_accept(old), -5)
        self.assertEqual(self.hop_offer(generation=2)[0], -5)
        self.assertEqual(self.call('apex_hop_init', HOP_K, CTX), 0)
        self.assertEqual(self.call('apex_hop_init', HOP_D, PEER), 0)
        self.assertLess(self.hop_accept(old), 0)
        self.assertEqual(self.hop_accept(self.hop_offer()[1]), 0)

    def link_init(self):
        self.handshake()
        for link, ctx in ((LINK_K, CTX), (LINK_D, PEER)):
            self.assertEqual(self.call('apex_hop_link_init', link, ctx, 74), 0)

    def link_next(self, now):
        n = self.call('fixture_hl_next', LINK_K, CTX, now, OUT, 88)
        return n, bytes(self.cpu.mem_read(OUT, n)) if n > 0 else b''

    def link_receive(self, link, ctx, wire, now):
        self.cpu.mem_write(IN, wire)
        n = self.call('fixture_hl_receive', link, ctx, IN, len(wire), now, OUT, 88)
        return n, bytes(self.cpu.mem_read(OUT, n)) if n > 0 else b''

    def link_map(self):
        self.link_init()
        self.assertEqual(self.link_next(1000)[0], 34)  # First offer lost.
        _, offer = self.link_next(6000)
        self.assertEqual(self.link_receive(LINK_D, PEER, offer, 14000)[0], 30)  # ACK lost.
        _, offer = self.link_next(11000)
        _, ack = self.link_receive(LINK_D, PEER, offer, 19000)
        self.assertEqual(self.link_receive(LINK_K, CTX, ack, 12000)[0], 0)

    def test_hop_start_lost_messages_and_blocked_channel(self):
        self.link_map()
        seen = set()
        sync_number = 0
        dropped = 0
        for now in range(17000, 412000, 5000):
            k = self.call('fixture_hl_channel', LINK_K, now)
            if now >= 112000:
                self.assertEqual(k, self.call('fixture_hl_channel', LINK_D, now + 8000))
                seen.add(k)
            if not self.call('fixture_hl_window', LINK_K, now):
                continue
            n, wire = self.link_next(now)
            self.assertGreaterEqual(n, 0)
            if not n:
                continue
            sync_number += 1
            if sync_number == 1 or (now >= 112000 and k == 50):
                dropped += 1
                continue
            n, ack = self.link_receive(LINK_D, PEER, wire, now + 8000)
            self.assertEqual(n, 34)
            if sync_number != 2:  # First received sync's ACK lost.
                self.assertEqual(self.link_receive(LINK_K, CTX, ack, now + 1000)[0], 0)
        self.assertEqual(seen, {6, 26, 50, 74})
        self.assertGreater(dropped, 1)

    def test_hop_start_no_ack_expires_without_counter_reset(self):
        self.link_map()
        previous_counter = 0
        for now in range(17000, 107001, 5000):
            n, wire = self.link_next(now)
            self.assertEqual(n, 36)
            counter = struct.unpack_from('<I', wire, 4)[0]
            self.assertGreater(counter, previous_counter)
            previous_counter = counter
            self.assertEqual(self.link_receive(LINK_D, PEER, wire, now + 8000)[0], 34)
        self.assertEqual(self.link_next(112000)[0], -6)
        self.assertEqual(self.call('fixture_hl_channel', LINK_K, 112000), -6)
        self.assertEqual(self.call('fixture_hl_channel', LINK_D, 215000), -6)

    def test_hop_discovery_visits_all_channels(self):
        for role, dwell in ((0, 300000), (1, 40000)):
            self.assertEqual([self.call('fixture_discovery', role, i * dwell) for i in range(5)],
                             [74, 6, 50, 26, 74])
        for offset in (0, 39999, 80000, 159999, 299999):
            encounters = set()
            for now in range(0, 1200000, 5000):
                k = self.call('fixture_discovery', 0, now)
                d = self.call('fixture_discovery', 1, now + offset)
                if k == d:
                    encounters.add(k)
            self.assertEqual(encounters, {6, 26, 50, 74})

    def test_hop_sync_is_sent_in_each_slot_despite_window_drift(self):
        self.link_map()
        _, wire = self.link_next(17000)
        _, ack = self.link_receive(LINK_D, PEER, wire, 25000)
        self.assertEqual(self.link_receive(LINK_K, CTX, ack, 18000)[0], 0)
        # A late clock update in one slot must not suppress the early update
        # in the next slot, even though fewer than 20 ms have elapsed.
        for slot in range(1, 12):
            phase = 14500 if slot % 2 else 5200
            now = 12000 + slot * 20000 + phase
            n, wire = self.link_next(now)
            self.assertEqual(n, 36)
            n, ack = self.link_receive(LINK_D, PEER, wire, now + 8000)
            self.assertEqual(n, 34)
            self.assertEqual(self.link_receive(LINK_K, CTX, ack, now + 1000)[0], 0)

    def test_input_window_waits_for_start_and_keeps_hop_guards(self):
        self.link_map()
        self.assertEqual(self.call('fixture_hl_input_window', LINK_K, 17000), 0)
        self.assertEqual(self.call('fixture_hl_input_window', LINK_D, 25000), 0)
        _, wire = self.link_next(17000)
        _, ack = self.link_receive(LINK_D, PEER, wire, 25000)
        self.assertEqual(self.link_receive(LINK_K, CTX, ack, 18000)[0], 0)
        for slot in range(1, 8):
            now = 12000 + slot * 20000 + 5000
            _, wire = self.link_next(now)
            _, ack = self.link_receive(LINK_D, PEER, wire, now + 8000)
            self.assertEqual(self.link_receive(LINK_K, CTX, ack, now + 1000)[0], 0)
            for phase in (0, 749, 750, 12999, 13000, 18499, 18500, 19499, 19500, 19999):
                expected = int(slot >= 5 and 750 <= phase < 18500)
                for link, offset in ((LINK_K, 0), (LINK_D, 8000)):
                    t = 12000 + slot * 20000 + phase + offset
                    # The receiver cannot extrapolate backwards before its latest sync.
                    if link == LINK_D and phase < 5000:
                        continue
                    self.assertEqual(self.call('fixture_hl_reply_window', link, t),
                                     int(slot >= 5 and 750 <= phase < 19500))
                    self.assertEqual(self.call('fixture_hl_input_window', link, t), expected)
            if slot >= 5:
                self.assertEqual(self.call('fixture_hl_window', LINK_K,
                                          12000 + slot * 20000 + 13000), 0)
        self.assertEqual(self.call('fixture_hl_input_window', LINK_D, now + 108000), 0)

    def test_clock_ack_progress_requires_new_authenticated_sync(self):
        self.link_map()
        self.assertEqual(self.call('fixture_hl_acked_sync', LINK_K), 0)
        _, wire = self.link_next(17000)
        counter = struct.unpack_from('<I', wire, 4)[0]
        _, ack = self.link_receive(LINK_D, PEER, wire, 25000)
        bad = bytearray(ack)
        bad[-1] ^= 1
        self.assertLess(self.link_receive(LINK_K, CTX, bytes(bad), 18000)[0], 0)
        self.assertEqual(self.call('fixture_hl_acked_sync', LINK_K), 0)
        self.assertEqual(self.link_receive(LINK_K, CTX, ack, 18000)[0], 0)
        self.assertEqual(self.call('fixture_hl_acked_sync', LINK_K), counter)
        self.assertLess(self.link_receive(LINK_K, CTX, ack, 19000)[0], 0)
        self.assertEqual(self.call('fixture_hl_acked_sync', LINK_K), counter)
        _, wire = self.link_next(37000)
        newer = struct.unpack_from('<I', wire, 4)[0]
        _, ack = self.link_receive(LINK_D, PEER, wire, 45000)
        self.assertEqual(self.link_receive(LINK_K, CTX, ack, 38000)[0], 0)
        self.assertGreater(newer, counter)
        self.assertEqual(self.call('fixture_hl_acked_sync', LINK_K), newer)
        # A peer may repeat an old ACK in a fresh encrypted packet.
        for acknowledged in (counter, newer):
            payload = bytes((0x49, 1)) + struct.pack('<II', 1, acknowledged)
            self.cpu.mem_write(DATA, payload)
            n = self.call('apex_connection_encode', PEER, 3, DATA, len(payload), OUT, 88)
            self.assertGreater(n, 0)
            repeat = bytes(self.cpu.mem_read(OUT, n))
            self.assertEqual(self.link_receive(LINK_K, CTX, repeat, 39000)[0], 0)
            self.assertEqual(self.call('fixture_hl_acked_sync', LINK_K), newer)

    def test_sync_correction_limit(self):
        for correction in (-501, -500, 500, 501):
            self.hop_init()
            self.assertEqual(self.hop_accept(self.hop_offer()[1]), 0)
            self.assertEqual(self.call('fixture_hop_begin', HOP_K, 0, 0, 0), 0)
            self.assertEqual(self.hop_receive(self.hop_sync(1000), 1000), 0)
            self.assertEqual(self.hop_receive(self.hop_sync(21000), 21000 + correction),
                             0 if abs(correction) <= 500 else -1)

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

    def test_authenticated_input_batch(self):
        payload = bytes([2, 3]) + b''.join(
            bytes([2]) + struct.pack('<I', sequence) + bytes([kind]) + bytes(size)
            for sequence, kind, size in ((1, 1, 8), (2, 2, 12), (3, 1, 8)))
        wire = self.encode(payload, packet_type=6)
        altered = bytearray(wire)
        altered[1] = 1
        self.assertEqual(self.decode(bytes(altered)), -2)
        self.assertEqual(self.decode(wire), len(payload))
        self.assertEqual(bytes(self.cpu.mem_read(TYPE, 1)), b'\x06')
        self.assertEqual(self.call('apex_delivery_batch_unpack', DATA, len(payload), 0x2000b000), 3)
        self.assertEqual(self.decode(wire), -3)

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
        expected = bytes([2]) + struct.pack('<I', 3) + bytes([1]) + press
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
        for bad in (wire[:-1], wire + b'\x00', b'\x01' + wire[1:],
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
               str(radio / 'src/apex_delivery.c'),
               str(radio / 'src/apex_hop.c'),
               str(radio / 'src/apex_hop_link.c'),
               str(radio / 'tests/packet_fixture.c')]
    command += [str(tiny / 'source' / n) for n in ('aes_encrypt.c', 'ccm_mode.c', 'hmac.c', 'sha256.c', 'utils.c')]
    subprocess.run(command + ['-o', str(binary)], check=True)
    with binary.open('rb') as f:
        elf = ELFFile(f)
        SYMBOLS = {s.name: int(s['st_value']) for s in elf.get_section_by_name('.symtab').iter_symbols()}
        SEGMENTS = [(int(s['p_vaddr']), s.data()) for s in elf.iter_segments()
                    if s['p_type'] == 'PT_LOAD' and s['p_filesz']]
    unittest.main(argv=['packet-tests'], verbosity=2)
