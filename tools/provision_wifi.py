# SPDX-FileCopyrightText: 2026 Marcel Verpaalen
# SPDX-License-Identifier: Apache-2.0
"""Flash WiFi credentials into a seatalk-bridge device's NVS (espOS wifi.ssid0/psk0).

usage (inside the ESP-IDF env, see tools/idf-env.ps1):
    python tools/provision_wifi.py --port COM3 [--secrets ../st50-emulator/secrets.ini]

Reads wifi_ssid / wifi_password from the [secrets] section of a secrets.ini
(the st50-emulator's by default), builds an NVS partition image in a temp
folder outside the repository (espos/docs/wifi.md, "Setting WiFi without the
portal"), writes it at the nvs offset and deletes the temp files. The values
are never printed.

Note: this replaces the whole nvs partition, so other settings stored on the
device (espOS config, boot count) go back to their defaults.
"""
import argparse
import configparser
import csv
import os
import shutil
import subprocess
import sys
import tempfile

NVS_OFFSET = "0x9000"  # espOS 4mb.csv
NVS_SIZE = "0x20000"

here = os.path.dirname(os.path.abspath(__file__))
ap = argparse.ArgumentParser()
ap.add_argument("--port", required=True)
ap.add_argument("--secrets", default=os.path.join(here, "..", "..", "st50-emulator", "secrets.ini"))
ap.add_argument("--baud", default="115200")
a = ap.parse_args()

cfg = configparser.ConfigParser()
if not cfg.read(a.secrets) or not cfg.has_section("secrets"):
    sys.exit(f"no [secrets] section in {a.secrets}")
ssid = cfg.get("secrets", "wifi_ssid", fallback="").strip()
psk = cfg.get("secrets", "wifi_password", fallback="").strip()
if not ssid:
    sys.exit("wifi_ssid is empty")
print(f"read WiFi settings from {os.path.abspath(a.secrets)} (SSID {len(ssid)} chars, password {len(psk)} chars)")

tmp = tempfile.mkdtemp(prefix="st-nvs-")
try:
    csv_path = os.path.join(tmp, "wifi.nvs.csv")
    bin_path = os.path.join(tmp, "wifi.nvs.bin")
    with open(csv_path, "w", newline="", encoding="utf-8") as f:
        w = csv.writer(f)
        w.writerow(["key", "type", "encoding", "value"])
        w.writerow(["wifi", "namespace", "", ""])
        w.writerow(["ssid0", "data", "string", ssid])
        w.writerow(["psk0", "data", "string", psk])
    subprocess.run([sys.executable, "-m", "esp_idf_nvs_partition_gen", "generate", csv_path, bin_path, NVS_SIZE],
                   check=True, stdout=subprocess.DEVNULL)
    subprocess.run([sys.executable, "-m", "esptool", "-p", a.port, "--chip", "esp32", "--connect-attempts", "0",
                    "-b", a.baud, "write-flash", NVS_OFFSET, bin_path], check=True)
    print("WiFi credentials written; the device restarts and joins the network.")
finally:
    shutil.rmtree(tmp, ignore_errors=True)
