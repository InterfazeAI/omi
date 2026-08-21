#!/usr/bin/env python3
"""Unit tests for check_ble_handler_filesystem.py (stdlib unittest)."""

from __future__ import annotations

import unittest

from check_ble_handler_filesystem import analyze, find_ble_callbacks, is_triggered, parse_functions, strip_comments

DEVKIT = "omi/firmware/devkit/src"
OMI = "omi/firmware/omi/src"

REGISTRATION = """
BT_GATT_SERVICE_DEFINE(demo_service,
    BT_GATT_PRIMARY_SERVICE(&service_uuid),
    BT_GATT_CHARACTERISTIC(&char_uuid.uuid, BT_GATT_CHRC_WRITE, BT_GATT_PERM_WRITE,
                           NULL, demo_write_handler, NULL),
);
"""

HANDLER_CALLING = """
static ssize_t demo_write_handler(struct bt_conn *conn,
                                  const struct bt_gatt_attr *attr,
                                  const void *buf,
                                  uint16_t len)
{
    return %s();
}
"""

CHEAP_ACCESSOR = """
uint32_t cheap_accessor(void)
{
    return cached_counter;
}
"""

CARD_READER = """
uint32_t read_the_card(uint8_t num)
{
    struct fs_dirent entry;
    return fs_stat("/SD:/audio/a1.txt", &entry) ? 0 : entry.size;
}
"""


class ParsingTests(unittest.TestCase):
    def test_parses_definition_with_multiline_parameters(self) -> None:
        functions = parse_functions(HANDLER_CALLING % "cheap_accessor")
        self.assertIn("demo_write_handler", functions)
        self.assertIn("cheap_accessor()", functions["demo_write_handler"])

    def test_ignores_declarations_that_have_no_body(self) -> None:
        self.assertEqual(parse_functions("int only_declared(void);\n"), {})

    def test_strip_comments_keeps_line_structure(self) -> None:
        stripped = strip_comments("int a(void)\n/* two\n   lines */\n{\n    return 0;\n}\n")
        self.assertEqual(stripped.count("\n"), 6)
        self.assertIn("a", parse_functions(stripped))

    def test_commented_out_filesystem_call_is_not_a_violation(self) -> None:
        source = "int handler_body(void)\n{\n    // fs_stat(path, &entry);\n    return 0;\n}\n"
        self.assertNotIn("fs_stat", strip_comments(source))

    def test_finds_callback_registered_in_gatt_macro(self) -> None:
        known = {"demo_write_handler"}
        self.assertEqual(find_ble_callbacks(REGISTRATION, known), known)

    def test_finds_connection_callback(self) -> None:
        source = "static struct bt_conn_cb callbacks = {\n    .connected = on_link_up,\n};\n"
        self.assertEqual(find_ble_callbacks(source, {"on_link_up"}), {"on_link_up"})


class AnalysisTests(unittest.TestCase):
    def test_accepts_handler_that_only_reads_memory(self) -> None:
        sources = {f"{DEVKIT}/demo.c": REGISTRATION + HANDLER_CALLING % "cheap_accessor" + CHEAP_ACCESSOR}
        self.assertEqual(analyze(sources), [])

    def test_rejects_handler_that_reaches_the_card(self) -> None:
        sources = {f"{DEVKIT}/demo.c": REGISTRATION + HANDLER_CALLING % "read_the_card" + CARD_READER}
        errors = analyze(sources)
        self.assertEqual(len(errors), 1)
        self.assertIn("demo_write_handler", errors[0])
        self.assertIn("read_the_card", errors[0])

    def test_follows_the_chain_across_files(self) -> None:
        """The time-sync bug crossed transport.c -> rtc.c -> sdcard.c."""
        sources = {
            f"{DEVKIT}/transport.c": REGISTRATION + HANDLER_CALLING % "set_the_clock",
            f"{DEVKIT}/rtc.c": "int set_the_clock(void)\n{\n    return mark_the_index();\n}\n",
            f"{DEVKIT}/sdcard.c": "int mark_the_index(void)\n{\n    return fs_write(&f, rec, 16);\n}\n",
        }
        errors = analyze(sources)
        self.assertEqual(len(errors), 1)
        self.assertIn("set_the_clock -> mark_the_index", errors[0])

    def test_accepts_work_latched_for_another_thread(self) -> None:
        """The actual fix: the handler records a request and returns."""
        sources = {
            f"{DEVKIT}/transport.c": REGISTRATION + HANDLER_CALLING % "request_mark",
            f"{DEVKIT}/sdcard.c": (
                "void request_mark(void)\n{\n    atomic_or(&pending, 1);\n}\n"
                "void service_mark(void)\n{\n    fs_write(&f, rec, 16);\n}\n"
            ),
        }
        self.assertEqual(analyze(sources), [])

    def test_a_clean_namesake_in_the_other_image_cannot_mask_a_violation(self) -> None:
        """devkit and omi share 104 function names; a shared graph hid a real bug."""
        sources = {
            f"{DEVKIT}/demo.c": REGISTRATION + HANDLER_CALLING % "read_the_card" + CARD_READER,
            f"{OMI}/demo.c": "uint32_t read_the_card(uint8_t num)\n{\n    return 0;\n}\n",
        }
        errors = analyze(sources)
        self.assertEqual(len(errors), 1)
        self.assertIn(DEVKIT, errors[0])


class TriggerTests(unittest.TestCase):
    def test_triggers_on_firmware_sources(self) -> None:
        self.assertTrue(is_triggered([f"{DEVKIT}/storage.c"]))
        self.assertTrue(is_triggered([f"{OMI}/lib/core/transport.c"]))

    def test_does_not_trigger_on_unrelated_changes(self) -> None:
        self.assertFalse(is_triggered(["app/lib/main.dart", "backend/routers/chat.py"]))


if __name__ == "__main__":
    unittest.main()
