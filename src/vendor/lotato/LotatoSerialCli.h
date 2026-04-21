#pragma once

/** USB-Serial CLI reply line: `  -> ` prefix, drip bytes with yield (ESP32), CRLF end. */
void lotato_serial_print_mesh_cli_reply(const char* reply);
