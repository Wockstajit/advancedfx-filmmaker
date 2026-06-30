# CS2 Offsets (sezzyaep/CS2-OFFSETS)

Flattened dump from [sezzyaep/CS2-OFFSETS](https://github.com/sezzyaep/CS2-OFFSETS).

## Files

| File | Contents |
|------|----------|
| `cs2-offsets-all.csv` | Everything in one sheet |
| `cs2-offsets-globals.csv` | Module globals (`dwEntityList`, `dwLocalPlayerPawn`, …) |
| `cs2-offsets-interfaces.csv` | Engine/client interface pointers |
| `cs2-offsets-buttons.csv` | Input button addresses |
| `cs2-offsets-schema-fields.csv` | Schema class field offsets (`m_iHealth`, `m_iItemDefinitionIndex`, …) |
| `cs2-offsets-enums.csv` | Schema enum member values |
| `cs2-offsets-meta.json` | Build number, dump timestamp, row counts |
| `source/*.json` | Raw JSON from the upstream repo |

## CSV columns

`category`, `module`, `class`, `name`, `offset_decimal`, `offset_hex`, `type`, `parent`

- **global** — flat module offset (no class)
- **interface** — exported interface RVA
- **button** — input button address
- **schema_field** — field offset inside a schema class
- **enum_member** — enum constant value (not a memory offset)

## Refresh

```batch
cd misc\cs2-offsets
python json_to_csv.py
```

Re-download `source/*.json` from upstream first if you need a newer game build.
