#!/bin/bash
#
# Build the RCO resource file on Linux using isage/acdc (drop-in for Sony acdc.exe).
# Build acdc first: cmake -B acdc-tool/build acdc-tool && cmake --build acdc-tool/build
# Expected at: <repo_root>/acdc-tool/build/acdc (override with ACDC env var)
#
set -e

PLUGIN="fpsvita"
REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
ACDC="${ACDC:-${REPO_ROOT}/acdc-tool/build/acdc}"
SCHEMA_DIR="${SCHEMA_DIR:-${REPO_ROOT}/acdc-tool/schema}"
TMP="RES_RCO_TMP"

cd "$(dirname "$0")"

rm -rf "$TMP"
mkdir -p "$TMP"

# Compile locale string file to RCS
"$ACDC" -c \
    -i "locale/fpsvita_locale_en.xml" \
    -s "${SCHEMA_DIR}/rcs.cxmldef" \
    -o "${TMP}/fpsvita_locale_en.xml.rcs"

# Compile main plugin XML to RCO
"$ACDC" -c \
    -i "${PLUGIN}_plugin.xml" \
    -s "${SCHEMA_DIR}/rco.cxmldef" \
    -o "${PLUGIN}_plugin.rco"

echo "Built ${PLUGIN}_plugin.rco"
