# Charset Table Generation

This directory contains the local pipeline for regenerating selected single-byte
character set tables used by `src/charset/generated/sbcs_to_unicode_selected.hpp`,
GB18030 tables used by `src/charset/generated/gb18030_tables.hpp`, KS X 1001
tables used by `src/charset/generated/ksx1001_tables.hpp`, and JIS X tables
used by `src/charset/generated/jisx0208_tables.hpp` and
`src/charset/generated/jisx0212_tables.hpp`.

## Generated Output

- `src/charset/generated/sbcs_to_unicode_selected.hpp`
- `src/charset/generated/gb18030_tables.hpp`
- `src/charset/generated/ksx1001_tables.hpp`
- `src/charset/generated/jisx0208_tables.hpp`
- `src/charset/generated/jisx0212_tables.hpp`

## Source Origin

The current selected tables come from the Unicode Consortium mapping files:

- `https://www.unicode.org/Public/MAPPINGS/ISO8859/8859-2.TXT`
  - Used for `ISO_IR 101` (Latin alphabet No. 2)
- `https://www.unicode.org/Public/MAPPINGS/ISO8859/8859-3.TXT`
  - Used for `ISO_IR 109` (Latin alphabet No. 3)
- `https://www.unicode.org/Public/MAPPINGS/ISO8859/8859-4.TXT`
  - Used for `ISO_IR 110` (Latin alphabet No. 4)
- `https://www.unicode.org/Public/MAPPINGS/ISO8859/8859-5.TXT`
  - Used for `ISO_IR 144` (Cyrillic)
- `https://www.unicode.org/Public/MAPPINGS/ISO8859/8859-6.TXT`
  - Used for `ISO_IR 127` (Arabic)
- `https://www.unicode.org/Public/MAPPINGS/ISO8859/8859-7.TXT`
  - Used for `ISO_IR 126` (Greek)
- `https://www.unicode.org/Public/MAPPINGS/ISO8859/8859-8.TXT`
  - Used for `ISO_IR 138` (Hebrew)
- `https://www.unicode.org/Public/MAPPINGS/ISO8859/8859-9.TXT`
  - Used for `ISO_IR 148` (Latin alphabet No. 5)
- `https://www.unicode.org/Public/MAPPINGS/ISO8859/8859-11.TXT`
  - Used for `ISO_IR 166` (Thai)
- `https://www.unicode.org/Public/MAPPINGS/ISO8859/8859-15.TXT`
  - Used for `ISO_IR 203` (Latin alphabet No. 9)

The GB18030 table comes from the ICU mapping XML:

- `https://raw.githubusercontent.com/unicode-org/icu-data/main/charset/data/xml/gb-18030-2000.xml`

The KS X 1001 table comes from the Unicode mapping file:

- `https://www.unicode.org/Public/MAPPINGS/OBSOLETE/EASTASIA/KSC/KSX1001.TXT`

The JIS tables come from the Unicode mapping files:

- `https://www.unicode.org/Public/MAPPINGS/OBSOLETE/EASTASIA/JIS/JIS0208.TXT`
- `https://www.unicode.org/Public/MAPPINGS/OBSOLETE/EASTASIA/JIS/JIS0212.TXT`

`ISO_IR 13` / `ISO 2022 IR 13` currently use a codec-local JIS X 0201 half-width
Katakana mapping. There is no separate generated header for that mapping yet.

For `ISO 2022 IR 58`, the implementation derives the GB2312 subset from the
existing GB18030 mapping source instead of maintaining a separate generated
header.

## Regeneration

Run:

```powershell
C:/msys64/clang64/bin/python3.exe misc/charset/generate_selected_sbcs_tables.py
C:/msys64/clang64/bin/python3.exe misc/charset/generate_gb18030_tables.py
C:/msys64/clang64/bin/python3.exe misc/charset/generate_ksx1001_tables.py
C:/msys64/clang64/bin/python3.exe misc/charset/generate_jis_tables.py
```

The script will:

1. Download the required Unicode mapping files into `misc/charset/data/unicode/`
   if they do not already exist.
2. Parse the `0x80..0xFF` byte range from each mapping file.
3. Generate `src/charset/generated/sbcs_to_unicode_selected.hpp`.

The GB18030 script will:

1. Download `gb-18030-2000.xml` into `misc/charset/data/icu/` if it does not already exist.
2. Parse the ICU XML assignments and ranges.
3. Generate `src/charset/generated/gb18030_tables.hpp`.

The KS X 1001 script will:

1. Download `KSX1001.TXT` into `misc/charset/data/unicode/` if it does not already exist.
2. Parse the Unicode mapping file into bidirectional lookup tables.
3. Generate `src/charset/generated/ksx1001_tables.hpp`.

The JIS script will:

1. Download `JIS0208.TXT` and `JIS0212.TXT` into `misc/charset/data/unicode/` if they do not already exist.
2. Parse the Unicode mapping files into bidirectional lookup tables.
3. Generate `src/charset/generated/jisx0208_tables.hpp` and `src/charset/generated/jisx0212_tables.hpp`.

Each generator writes its output only when the generated content actually changes.

## Regression Check

To verify that the checked-in generated headers still match the current scripts, run:

```powershell
ctest --test-dir build-msyscheck -R charset_generator_regression --output-on-failure
```

This test reruns all charset generators and fails if any tracked generated header
would change. The test restores the original files before exiting, so it does not
leave the worktree dirty on failure.

## Notes

- The generated header intentionally contains the subset currently supported by
  the runtime codec. It now covers the single-byte registry terms implemented in
  the new writer/read-path implementation.
- `ISO_IR 192` uses the native UTF-8 path and does not need generated tables.
- `ISO_IR 100` uses a direct Latin-1 path in the codec and does not need a
  generated table.
- `GBK` reuses the generated GB18030 forward/reverse lookup data instead of
  maintaining a separate GBK header.
- `ISO_IR 13` / `ISO 2022 IR 13` still use codec-local mapping logic rather than
  a generated table.
- If new SBCS or ISO 2022 aliases are added, extend the selected mapping source
  list here first, then regenerate the header and update the smoke tests.
- Current ISO 2022 writer policy assumes the initial designated code element is
  already active at the start of each value.
- For single-term ISO 2022 charsets, this means the writer omits the first
  designation escape for the initial G0/G1 code element.
- After PN component delimiters (`^`, `=`) and after value delimiters (`\\`),
  the writer resets back to the declared initial ISO 2022 state and does not
  emit a repeated designation escape for that initial state.
- The reader still accepts explicit escape sequences when they are present in
  incoming raw DICOM bytes.
- The reader also accepts supported ISO 2022 escape sequences even when they
  were not explicitly declared in `(0008,0005)`. Unknown escape sequences are
  still rejected.
- Multi-term `(0008,0005)` values that are not pure ISO 2022 combinations are
  rejected.
