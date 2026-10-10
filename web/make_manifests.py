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
# ESP Launchpad (Quick Start page): format of espressif/esp-launchpad config/config.toml. Each flash
# size is an app of its own; firmware_images_url must be absolute and the image paths hang off it.
base = sys.argv[2] if len(sys.argv) > 2 else ""
if not base and os.environ.get("GITHUB_REPOSITORY"):
    owner, name = os.environ["GITHUB_REPOSITORY"].split("/", 1)
    base = f"https://{owner.lower()}.github.io/{name}/"
base = base.rstrip("/") + "/fw/"
LAUNCHPAD_KEY = {"esp32": "esp32", "esp32s3": "esp32-s3", "esp32c6": "esp32-c6"}
apps = {}
for size in ("4mb", "8mb"):
    chips = [c for c in FAMILY if os.path.exists(os.path.join(site, f"fw/{c}/factory-{size}.bin"))]
    if chips:
        apps[f"esp-wifi-loader-{size}"] = (size, chips)
lines = ["esp_toml_version = 1.0", f'firmware_images_url = "{base}"',
         "supported_apps = [" + ", ".join(f'"{a}"' for a in apps) + "]", ""]
for app, (size, chips) in apps.items():
    lines.append(f"[{app}]")
    lines.append("chipsets = [" + ", ".join(f'"{FAMILY[c]}"' for c in chips) + "]")
    for c in chips:
        lines.append(f'image.{LAUNCHPAD_KEY[c]} = "{c}/factory-{size}.bin"')
    lines.append(f'description = "esp-wifi-loader for boards with {size.replace("mb", " MB")} of flash: Stage2, partition table and loader in one image, written at offset 0."')
    lines.append("")
with open(os.path.join(site, "launchpad.toml"), "w") as f:
    f.write("\n".join(lines))
