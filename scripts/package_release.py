import argparse
import json
import shutil
from pathlib import Path


TARGETS = (
    ("esp32dev", "ESP32", 0x1000),
    ("esp32-c3-super-mini", "ESP32-C3", 0x0000),
    ("esp32-s2-mini", "ESP32-S2", 0x1000),
)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--version", required=True)
    parser.add_argument("--artifacts", type=Path, required=True)
    parser.add_argument("--site", type=Path, required=True)
    args = parser.parse_args()

    firmware_dir = args.site / "firmware"
    firmware_dir.mkdir(parents=True, exist_ok=True)
    builds = []

    for environment, chip_family, bootloader_offset in TARGETS:
        parts = (
            ("bootloader", bootloader_offset),
            ("partitions", 0x8000),
            ("boot_app0", 0xE000),
            ("ota", 0x10000),
        )
        manifest_parts = []

        for image, offset in parts:
            filename = f"{environment}-{image}.bin"
            source = args.artifacts / filename
            if not source.is_file():
                raise FileNotFoundError(f"Missing release image: {source}")
            shutil.copy2(source, firmware_dir / filename)
            manifest_parts.append(
                {"path": f"firmware/{filename}", "offset": offset}
            )

        builds.append(
            {
                "chipFamily": chip_family,
                "improv": False,
                "parts": manifest_parts,
            }
        )

    manifest = {
        "name": "LaCrosse MQTT Gateway",
        "version": args.version,
        "new_install_prompt_erase": True,
        "builds": builds,
    }
    (args.site / "manifest.json").write_text(
        json.dumps(manifest, indent=2) + "\n", encoding="utf-8"
    )
    shutil.copy2(Path("web-flasher") / "index.html", args.site / "index.html")
    shutil.copy2(Path("favicon.ico"), args.site / "favicon.ico")


if __name__ == "__main__":
    main()
