#!/usr/bin/env python3
"""Flatten sezzyaep/CS2-OFFSETS JSON dumps into CSV files."""

from __future__ import annotations

import csv
import json
from pathlib import Path

SOURCE = Path(__file__).resolve().parent / "source"
OUT = Path(__file__).resolve().parent

COLUMNS = [
    "category",
    "module",
    "class",
    "name",
    "offset_decimal",
    "offset_hex",
    "type",
    "parent",
]


def load_json(name: str) -> dict:
    path = SOURCE / name
    with path.open(encoding="utf-8") as f:
        return json.load(f)


def write_rows(path: Path, rows: list[dict]) -> None:
    with path.open("w", newline="", encoding="utf-8") as f:
        writer = csv.DictWriter(f, fieldnames=COLUMNS)
        writer.writeheader()
        writer.writerows(rows)


def row(
    category: str,
    module: str,
    class_name: str,
    name: str,
    offset: int,
    type_name: str = "",
    parent: str = "",
) -> dict:
    return {
        "category": category,
        "module": module,
        "class": class_name,
        "name": name,
        "offset_decimal": offset,
        "offset_hex": f"0x{offset:X}",
        "type": type_name,
        "parent": parent or "",
    }


def flatten_globals() -> list[dict]:
    data = load_json("offsets.json")
    rows: list[dict] = []
    for module, entries in data.items():
        for name, offset in entries.items():
            rows.append(row("global", module, "", name, int(offset)))
    return rows


def flatten_interfaces() -> list[dict]:
    data = load_json("interfaces.json")
    rows: list[dict] = []
    for module, entries in data.items():
        for name, offset in entries.items():
            rows.append(row("interface", module, "", name, int(offset)))
    return rows


def flatten_buttons() -> list[dict]:
    data = load_json("buttons.json")
    rows: list[dict] = []
    for module, entries in data.items():
        for name, offset in entries.items():
            rows.append(row("button", module, "", name, int(offset)))
    return rows


def flatten_schemas() -> tuple[list[dict], list[dict]]:
    data = load_json("schemas.json")
    field_rows: list[dict] = []
    enum_rows: list[dict] = []

    for module, module_data in data.items():
        classes = module_data.get("classes", {})
        for class_name, class_data in classes.items():
            parent = class_data.get("parent") or ""
            for field_name, field_data in class_data.get("fields", {}).items():
                field_rows.append(
                    row(
                        "schema_field",
                        module,
                        class_name,
                        field_name,
                        int(field_data["offset"]),
                        field_data.get("type", ""),
                        parent,
                    )
                )

        enums = module_data.get("enums", {})
        for enum_name, enum_data in enums.items():
            enum_type = enum_data.get("type", "")
            for member_name, member_value in enum_data.get("members", {}).items():
                enum_rows.append(
                    row(
                        "enum_member",
                        module,
                        enum_name,
                        member_name,
                        int(member_value),
                        enum_type,
                    )
                )

    return field_rows, enum_rows


def main() -> None:
    globals_rows = flatten_globals()
    interfaces_rows = flatten_interfaces()
    buttons_rows = flatten_buttons()
    schema_rows, enum_rows = flatten_schemas()

    write_rows(OUT / "cs2-offsets-globals.csv", globals_rows)
    write_rows(OUT / "cs2-offsets-interfaces.csv", interfaces_rows)
    write_rows(OUT / "cs2-offsets-buttons.csv", buttons_rows)
    write_rows(OUT / "cs2-offsets-schema-fields.csv", schema_rows)
    write_rows(OUT / "cs2-offsets-enums.csv", enum_rows)

    all_rows = (
        globals_rows
        + interfaces_rows
        + buttons_rows
        + schema_rows
        + enum_rows
    )
    write_rows(OUT / "cs2-offsets-all.csv", all_rows)

    info = load_json("info.json")
    meta = {
        "source": "https://github.com/sezzyaep/CS2-OFFSETS",
        "generator": info.get("generator"),
        "build_number": info.get("build_number"),
        "timestamp": info.get("timestamp"),
        "counts": {
            "globals": len(globals_rows),
            "interfaces": len(interfaces_rows),
            "buttons": len(buttons_rows),
            "schema_fields": len(schema_rows),
            "enum_members": len(enum_rows),
            "total": len(all_rows),
        },
    }
    with (OUT / "cs2-offsets-meta.json").open("w", encoding="utf-8") as f:
        json.dump(meta, f, indent=2)
        f.write("\n")

    print(json.dumps(meta, indent=2))


if __name__ == "__main__":
    main()
