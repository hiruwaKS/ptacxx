#!/bin/bash

# deps: ar, llvm-objcopy, llvm-link, llvm-dis, llvm-nm, od

set -eo pipefail
if [ -z "${LLVM_TOOLS_DIR}" ]; then
    echo "Error: LLVM_TOOLS_DIR environment variable is not set!" >&2
    exit 1
fi
LLVM_OBJCOPY="${LLVM_TOOLS_DIR}/llvm-objcopy"
LLVM_LINK="${LLVM_TOOLS_DIR}/llvm-link"
LLVM_DIS="${LLVM_TOOLS_DIR}/llvm-dis"
LLVM_NM="${LLVM_TOOLS_DIR}/llvm-nm"

if [ "$#" -eq 0 ]; then
    echo "Error: Please provide at least one .a archive file!"
    echo "Usage: $0 <path_to_archive.a> [path_to_archive.a ...]"
    exit 1
fi

INPUT_AS=("$@")
FIRST_INPUT_A="${INPUT_AS[0]}"
ABS_FIRST_INPUT_A=$(cd "$(dirname "$FIRST_INPUT_A")" && pwd)/$(basename "$FIRST_INPUT_A")
ABS_FIRST_INPUT_A_DIR=$(dirname "$ABS_FIRST_INPUT_A")
FIRST_A_NAME=$(basename "$FIRST_INPUT_A")
OUTPUT_BC_NAME="${FIRST_A_NAME%.*}.bc"
OUTPUT_BC_PATH="${ABS_FIRST_INPUT_A_DIR}/${OUTPUT_BC_NAME}"

ORIG_DIR="$(mktemp -d)"
UNZIP_DIRS=()
BC_FILES=()

cleanup() {
    for unzip_dir in "${UNZIP_DIRS[@]}"; do
        if [ -d "$unzip_dir" ]; then
            echo "--> Cleaning up directory: $unzip_dir"
            rm -rf "$unzip_dir"
        fi
    done
}
trap cleanup EXIT

# 1. Extract each archive and collect LLVM Bitcode files from its members.
for i in "${!INPUT_AS[@]}"; do
    INPUT_A="${INPUT_AS[$i]}"
    ABS_INPUT_A=$(cd "$(dirname "$INPUT_A")" && pwd)/$(basename "$INPUT_A")
    A_NAME=$(basename "$INPUT_A")
    UNZIP_DIR="${A_NAME}.unzip.${i}"
    FULL_UNZIP_DIR="${ORIG_DIR}/${UNZIP_DIR}"
    UNZIP_DIRS+=("$FULL_UNZIP_DIR")

    echo "==> Processing: $A_NAME"

    mkdir -p "$FULL_UNZIP_DIR"
    (
        cd "$FULL_UNZIP_DIR"
        ar x "$ABS_INPUT_A"
    )

    for f in "$FULL_UNZIP_DIR"/*; do
        [ -f "$f" ] || continue

        # Method 1: Try to dump bitcode section from object file (.llvmbc or __llvm_bc)
        if ${LLVM_OBJCOPY} --dump-section .llvmbc="${f}.extracted.bc" "$f" 2>/dev/null; then
            BC_FILES+=("${f}.extracted.bc")
            continue
        elif ${LLVM_OBJCOPY} --dump-section __llvm_bc="${f}.extracted.bc" "$f" 2>/dev/null; then
            BC_FILES+=("${f}.extracted.bc")
            continue
        fi

        # Method 2: Check magic bytes for LLVM bitcode (BC\x04\x15)
        # BC file magic: 0x42 0x43 0x04 0x15
        if [ "$(head -c 4 "$f" | od -An -tx1 | grep -cE "42 43 (c0 de|04 15)" 2>/dev/null)" -gt 0 ]; then
            BC_FILES+=("$f")
            continue
        fi

        # Method 3: Try to disassemble with llvm-dis as a final check
        if ${LLVM_DIS} "$f" -o /dev/null 2>/dev/null; then
            BC_FILES+=("$f")
        fi
    done
done

TOTAL_BC=${#BC_FILES[@]}

if [ "$TOTAL_BC" -eq 0 ]; then
    echo "Error: No LLVM Bitcode found in the provided archives! Make sure they were compiled with Clang and -flto."
    exit 1
fi

# 4. Link all Bitcode files into a single .bc
echo "--> Linking $TOTAL_BC bitcode files..."
${LLVM_LINK} --only-needed "${BC_FILES[@]}" -o "$OUTPUT_BC_PATH"

# 5. Check if 'main' function exists
MAIN_FOUND="No"
if [ "$(${LLVM_NM} "$OUTPUT_BC_PATH" 2>/dev/null | grep -cE ' T _?main$')" -gt 0 ]; then
    MAIN_FOUND="Yes"
elif [ "$(${LLVM_DIS} -o - "$OUTPUT_BC_PATH" 2>/dev/null | grep -cE 'define.*@main\(')" -gt 0 ]; then
    MAIN_FOUND="Yes"
fi

# 6. Report results
echo ""
echo "==================== Summary ===================="
echo "Extracted Bitcode Files : $TOTAL_BC"
echo "Contains 'main' Function: $MAIN_FOUND"
echo "Output File             : $OUTPUT_BC_PATH"
echo "================================================="