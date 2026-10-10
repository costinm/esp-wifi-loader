#!/usr/bin/env python3
"""Write ESP Web Tools manifests and the ESP Launchpad config into SITE_DIR."""
import json
import os
import sys

site = sys.argv[1]
FAMILY = {"esp32": "ESP32", "esp32s3": "ESP32-S3", "esp32c6": "ESP32-C6"}
for cpu, family in FAMILY.items():
    for size in ("4mb", "8mb"):
        name = f"fw/{cpu}/factory-{size}.bin"
        if not os.path.exists(os.path.join(site, name)):
            continue
        manifest = {"name": f"esp-wifi-loader ({family}, {size})", "new_install_prompt_erase": True,
                    "builds": [{"chipFamily": family, "parts": [{"path": name, "offset": 0}]}]}
        with open(os.path.join(site, f"manifest-{cpu}-{size}.json"), "w") as f:
            json.dump(manifest, f, indent=2)
lines = ['[esp-wifi-loader]', 'description = "Install the esp-wifi-loader (blank board)"']
for cpu in FAMILY:
    for size in ("4mb", "8mb"):
        if os.path.exists(os.path.join(site, f"fw/{cpu}/factory-{size}.bin")):
            lines.append(f'{cpu}_{size}.flash = "fw/{cpu}/factory-{size}.bin"')
with open(os.path.join(site, "launchpad.toml"), "w") as f:
    f.write("\n".join(lines) + "\n")
