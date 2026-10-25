"""Pure guards and simulated RAW benchmark tests: no SDK, serial or RF needed."""

from contextlib import redirect_stderr, redirect_stdout
import importlib.util
import io
import json
from pathlib import Path
import sys
from types import ModuleType, SimpleNamespace
import unittest
from unittest import mock


ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location("companion_raw_radio2_sweep_test",
    ROOT / "tools/hil/companion_raw_radio2_sweep.py")
BENCH = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(BENCH)


class FakePair:
    def __init__(self, *, loss=False, bad_guard=False, primary_tx=0, conflict=False, inactive=False,
                 initial_power=21, pending_reads=0):
        self.clock = 100.0
        self.last_source = None
        self.last_payload = None
        self.loss = loss
        self.total_sent = 0
        owner = self

        class Node:
            max_channels = 1

            def __init__(self, name):
                self.name, self.preamble = name, 128
                self.commands, self.requests = [], []
                self.tx = 0
                self.tx_power = initial_power
                self.applied_tx_power = 21 if pending_reads else initial_power
                self.pending_reads = pending_reads
                self.tx_verified = False

            def channel_name(self, slot):
                return "mt-conflict" if conflict else "Public"

            def cli(self, command):
                self.commands.append(command)
                if command == "get lab.tx":
                    return "> wrong guard" if bad_guard and self.name == "b" else "> " + BENCH.GUARD
                if command == "get radio":
                    return "> 910.5250244,62.5,7,5"
                if command == "get radio2.status":
                    return f"> RX=0,0 TX={primary_tx},{self.tx}; active=radio2"
                if command == "get tx":
                    return f"> {self.tx_power}"
                if command == "get tx.status":
                    if self.pending_reads:
                        self.pending_reads -= 1
                        return f"> desired={self.tx_power},apply=pending"
                    self.applied_tx_power = self.tx_power
                    self.tx_verified = self.applied_tx_power == 2
                    return f"> desired={self.tx_power},apply=applied"
                if command == "set tx 2":
                    self.tx_power = 2
                    return "OK - saved; pending" if self.pending_reads else "OK"
                if command.startswith("set radio2 "):
                    self.preamble = int(command.rsplit(",", 1)[1])
                if command == "get radio2":
                    return f"> 909.500000,500,5,5,rxtx,{self.preamble}; saved"
                if command == "get tx.ssh":
                    return "> tx.ssh radio2; active=" + ("off" if inactive else "radio2")
                if command == "get radio2.timing":
                    return "> synthetic timing only"
                return "OK"

            def request(self, command, expected):
                assert all(node.tx_verified and node.applied_tx_power == 2
                           for node in owner.nodes.values()), "RF before verified low TX power"
                self.requests.append(command)
                assert command[:2] == b"\x19\x00"
                assert expected == (0,)
                owner.last_source, owner.last_payload = self.name, command[2:]
                owner.total_sent += 1
                self.tx += 1
                return b"\x00"

        self.nodes = {name: Node(name) for name in ("a", "b")}

    def now(self):
        self.clock += 0.01
        return self.clock

    def receiver(self, destination, payload, timeout):
        assert destination.name != self.last_source
        assert payload == self.last_payload
        received = not (self.loss and self.total_sent % 3 == 0)
        return {"received": received, "completed_at": self.now() if received else None,
                "unexpected_raw": 0, "ignored_frames": 0}


class CompanionRawRadio2SweepTest(unittest.TestCase):
    def run_matrix(self, pair):
        checkpoints = []
        with mock.patch.object(BENCH.time, "sleep"), \
                mock.patch.object(BENCH.time, "monotonic", side_effect=pair.now):
            result = BENCH.run_matrix(pair.nodes, BENCH.PREAMBLES, (64, 172), count=2,
                timeout=1, interval=0, persist=lambda value: checkpoints.append(json.loads(json.dumps(value))),
                receiver=pair.receiver, run_id=b"TESTONLY")
        return result, checkpoints

    def test_exact_raw_sizes_and_distinct_marker_identity(self):
        first = BENCH.make_payload(b"TESTONLY", 1, 0, 172)
        self.assertEqual(len(first), 172)
        self.assertEqual(first[:4], b"SSH\x01")
        self.assertNotEqual(first, BENCH.make_payload(b"TESTONLY", 2, 0, 172))
        self.assertNotEqual(first, BENCH.make_payload(b"TESTONLY", 1, 1, 172))
        with self.assertRaisesRegex(BENCH.Failure, "172"):
            BENCH.make_payload(b"TESTONLY", 1, 0, 230)

    def test_guard_primary_and_profile_counters_are_strict(self):
        BENCH.prove_guard("> " + BENCH.GUARD)
        self.assertEqual(BENCH.parse_primary("> 910.5250244,62.5,7,5")[1:], (62.5, 7, 5))
        self.assertEqual(BENCH.parse_tx("> TX=0,12; active"), [0, 12])
        for function, value in ((BENCH.prove_guard, "> HIL TX lock=910.525 MHz"),
                                (BENCH.parse_primary, "> 909.5,500,5,5"),
                                (BENCH.parse_tx, "> no counters")):
            with self.subTest(value=value), self.assertRaises(BENCH.Failure):
                function(value)

    def test_all_cases_count_opposite_exact_receipt_not_just_command_ok(self):
        pair = FakePair()
        result, checkpoints = self.run_matrix(pair)
        self.assertEqual(result["result"], "PASS", result.get("failure"))
        self.assertEqual(len(result["cases"]), 8)
        self.assertEqual(pair.total_sent, 32)
        self.assertEqual({item["raw_bytes"] for item in result["cases"] if item["preamble"] == 384}, {64, 172})
        self.assertTrue(all(item["measurement"]["received"] == 4 for item in result["cases"]))
        self.assertGreater(len(checkpoints), pair.total_sent)
        self.assertFalse(result["mtu230_supported"])
        self.assertTrue(all(not any(command.startswith(("set radio ", "set wifi", "set mt.channel"))
                                   for command in node.commands) for node in pair.nodes.values()))

    def test_rf_loss_is_reported_and_not_hidden_by_command_ok(self):
        result, _ = self.run_matrix(FakePair(loss=True))
        self.assertEqual(result["result"], "LOSS")
        self.assertGreater(sum(item["measurement"]["lost"] for item in result["cases"]), 0)
        self.assertTrue(all(packet["command_completed"] for case in result["cases"]
                            for packet in case["transfers"]))
        self.assertEqual(result["retries"], 0)

    def test_guard_failure_primary_tx_or_channel_conflict_prevents_all_setters(self):
        for pair in (FakePair(bad_guard=True), FakePair(primary_tx=1), FakePair(conflict=True)):
            with self.subTest(pair=pair):
                result, _ = self.run_matrix(pair)
                self.assertEqual(result["result"], "FAIL")
                self.assertEqual(pair.total_sent, 0)
                self.assertFalse(any(command.startswith(("set ", "normalradio"))
                                     for node in pair.nodes.values() for command in node.commands))

    def test_saved_but_inactive_radio2_route_prevents_transmission(self):
        pair = FakePair(inactive=True)
        result, _ = self.run_matrix(pair)
        self.assertEqual(result["result"], "FAIL")
        self.assertEqual(pair.total_sent, 0)
        self.assertIn("saved and active", result["failure"]["message"])

    def test_temporary_profile_release_settles_before_tx_power_is_changed(self):
        pair = FakePair()
        events = []
        for name, node in pair.nodes.items():
            original = node.cli

            def command(text, *, name=name, original=original):
                events.append((name, text))
                return original(text)

            node.cli = command
        with mock.patch.object(BENCH.time, "sleep", side_effect=lambda value: events.append(("sleep", value))), \
                mock.patch.object(BENCH.time, "monotonic", side_effect=pair.now):
            result = BENCH.run_matrix(pair.nodes, (256,), (64,), count=1,
                timeout=1, interval=0, persist=lambda report: None, receiver=pair.receiver, run_id=b"TESTONLY")
        self.assertEqual(result["result"], "PASS")
        first_power = events.index(("a", "set tx 2"))
        releases_done = max(events.index((name, "set tempradio2 off")) for name in ("a", "b"))
        self.assertIn(("sleep", 2.5), events[releases_done + 1:first_power])

    def test_plan_and_invalid_230_size_never_load_hardware_helpers(self):
        for arguments, expected in ((["--plan"], 0), (["--plan", "--sizes", "230"], 2), ([], 2)):
            with self.subTest(arguments=arguments), redirect_stdout(io.StringIO()) as output, \
                    redirect_stderr(io.StringIO()), \
                    mock.patch.object(BENCH, "safe_identity", side_effect=AssertionError("hardware inventory")):
                try:
                    code = BENCH.main(arguments)
                except SystemExit as error:
                    code = error.code
                self.assertEqual(code, expected)
                if code == 0:
                    plan = json.loads(output.getvalue())
                    self.assertEqual(plan["total_packets"], 160)
                    self.assertEqual(plan["preambles"], [128, 256, 384, 512])

    def test_384_plan_is_accepted_but_unreviewed_or_duplicate_preambles_are_rejected(self):
        for preambles, expected in (("384", 0), ("128,384,512", 0), ("300", 2), ("384,384", 2)):
            with self.subTest(preambles=preambles), redirect_stdout(io.StringIO()) as output, \
                    redirect_stderr(io.StringIO()), \
                    mock.patch.object(BENCH, "safe_identity", side_effect=AssertionError("hardware inventory")):
                try:
                    code = BENCH.main(["--plan", "--preambles", preambles])
                except SystemExit as error:
                    code = error.code
                self.assertEqual(code, expected)
                if code == 0:
                    plan = json.loads(output.getvalue())
                    self.assertIn(384, plan["preambles"])

    def test_usb_identity_rejects_mismatched_stale_tty_without_open(self):
        listing = ModuleType("serial.tools")
        listing.list_ports = SimpleNamespace(comports=lambda: [SimpleNamespace(
            device="/dev/SIMULATED", serial_number="ACTUAL", vid=1, pid=2)])
        with mock.patch.dict(sys.modules, {"serial": ModuleType("serial"), "serial.tools": listing}):
            with self.assertRaisesRegex(BENCH.Failure, "expected physical serial"):
                BENCH.safe_identity("/dev/SIMULATED", "WRONG")
            self.assertEqual(BENCH.safe_identity("/dev/SIMULATED", "ACTUAL")["serial"], "ACTUAL")

    def test_raw_push_requires_exact_payload_bytes(self):
        class FrameTimeout(Exception):
            pass
        helper = ModuleType("esp32_companion_serial_stress")
        helper.FrameTimeout = FrameTimeout
        payload = BENCH.make_payload(b"TESTONLY", 1, 0, 64)
        node = SimpleNamespace(port=object(), reader=SimpleNamespace(read_frame=mock.Mock(
            side_effect=(b"\x83", b"\x84\x00\x00\xffwrong", b"\x84\x00\x00\xff" + payload))))
        with mock.patch.dict(sys.modules, {"esp32_companion_serial_stress": helper}):
            result = BENCH.receive_exact(node, payload, 1)
        self.assertTrue(result["received"])
        self.assertEqual(result["unexpected_raw"], 1)
        self.assertEqual(result["ignored_frames"], 1)

    def test_existing_low_tx_still_requires_applied_status(self):
        node = SimpleNamespace(cli=mock.Mock(side_effect=("> 2", "> desired=2,apply=applied")))
        result = BENCH.ensure_low_tx(node)
        self.assertEqual(result["setter_attempts"], 0)
        self.assertEqual(node.cli.call_args_list, [mock.call("get tx"), mock.call("get tx.status")])

    def test_pending_power_is_polled_without_resubmitting_same_or_changed_desired_value(self):
        for initial in (2, 21):
            with self.subTest(initial=initial):
                responses = [f"> {initial}"]
                if initial != 2:
                    responses.append("OK - saved; pending")
                responses += ["> desired=2,apply=pending"] * 2 + ["> desired=2,apply=applied"]
                node = SimpleNamespace(cli=mock.Mock(side_effect=responses))
                with mock.patch.object(BENCH.time, "sleep") as sleep:
                    result = BENCH.ensure_low_tx(node)
                expected = [mock.call("get tx")]
                if initial != 2:
                    expected.append(mock.call("set tx 2"))
                expected += [mock.call("get tx.status")] * 3
                self.assertEqual(node.cli.call_args_list, expected)
                self.assertEqual(result["setter_attempts"], int(initial != 2))
                self.assertEqual(result["status_polls"], 3)
                self.assertEqual(sleep.call_args_list, [mock.call(0.25)] * 2)

    def test_pending_apply_blocks_all_rf_until_both_radios_confirm_low_power(self):
        for initial in (2, 21):
            with self.subTest(initial=initial):
                pair = FakePair(initial_power=initial, pending_reads=2)
                result, _ = self.run_matrix(pair)
                self.assertEqual(result["result"], "PASS", result.get("failure"))
                self.assertEqual(pair.total_sent, 32)
                for node in pair.nodes.values():
                    self.assertEqual(node.commands.count("set tx 2"), int(initial != 2))
                    self.assertEqual(node.commands.count("get tx.status"), 3)

    def test_unapplied_power_times_out_without_setter_resubmission_or_rf(self):
        for initial in (2, 21):
            with self.subTest(initial=initial):
                pair = FakePair(initial_power=initial, pending_reads=1000)
                result, _ = self.run_matrix(pair)
                self.assertEqual(result["result"], "FAIL")
                self.assertIn("bounded setup window", result["failure"]["message"])
                self.assertEqual(pair.total_sent, 0)
                for node in pair.nodes.values():
                    self.assertLessEqual(node.commands.count("set tx 2"), int(initial != 2))
                    self.assertFalse(any(command.startswith("set radio2 ") for command in node.commands))
        node = SimpleNamespace(cli=mock.Mock(side_effect=("> 21", "OK - saved; pending",
            "> desired=2,apply=pending", "> desired=2,apply=pending")))
        with mock.patch.object(BENCH.time, "sleep"), \
                mock.patch.object(BENCH.time, "monotonic", side_effect=(0, 1, 8)):
            with self.assertRaisesRegex(BENCH.Failure, "bounded setup window"):
                BENCH.ensure_low_tx(node)
        self.assertEqual(node.cli.call_args_list.count(mock.call("set tx 2")), 1)

    def test_missing_malformed_or_contradictory_status_fails_closed(self):
        for initial in (2, 21):
            for status in ("OK", "unknown config", "", "> 2", "> desired=21,apply=applied",
                           "> desired=21,apply=pending", "> desired=2,apply=unknown",
                           "> desired=2,apply=applied; desired=21", "> desired=2,apply=applied\nOK"):
                with self.subTest(initial=initial, status=status):
                    pair = FakePair(initial_power=initial)
                    original = pair.nodes["b"].cli
                    pair.nodes["b"].cli = lambda command: status if command == "get tx.status" else original(command)
                    result, _ = self.run_matrix(pair)
                    self.assertEqual(result["result"], "FAIL")
                    self.assertEqual(pair.total_sent, 0)
                    self.assertFalse(any(command.startswith("set radio2 ")
                                         for node in pair.nodes.values() for command in node.commands))

    def test_unknown_tx_setter_errors_or_missing_numeric_readback_fail_closed(self):
        for responses in (("unknown config",),
                          ("> 21", RuntimeError("Companion CLI set tx rejected: Error: invalid TX power")),
                          ("> 21", "Error: radio busy or TX power rejected"),
                          ("> 21", "unknown config"), ("> 21", "OK but rejected")):
            with self.subTest(responses=responses):
                node = SimpleNamespace(cli=mock.Mock(side_effect=responses))
                with self.assertRaises(Exception), mock.patch.object(BENCH.time, "sleep") as sleep:
                    BENCH.ensure_low_tx(node)
                sleep.assert_not_called()

    def test_packet_window_rate_excludes_checkpoint_time_but_keeps_loss_timeouts(self):
        packets = [{"received": True, "raw_bytes": 64, "elapsed_seconds": 0.1, "direction": "a_to_b"},
                   {"received": False, "raw_bytes": 64, "elapsed_seconds": 1, "direction": "b_to_a"}]
        value = BENCH.metrics(packets, elapsed=5)
        self.assertAlmostEqual(value["application_goodput_kbps"], 60 * 8 / 5 / 1000)
        self.assertAlmostEqual(value["packet_window_application_goodput_kbps"], 60 * 8 / 1.1 / 1000)
        self.assertAlmostEqual(value["packet_window_seconds"], 1.1)


if __name__ == "__main__":
    unittest.main()
