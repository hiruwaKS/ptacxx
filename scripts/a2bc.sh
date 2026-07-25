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

if [ -z "$1" ]; then
    echo "Error: Please provide the path to a .a archive file!"
    echo "Usage: $0 <path_to_archive.a>"
    exit 1
fi
INPUT_A="$1"
ABS_INPUT_A=$(cd "$(dirname "$INPUT_A")" && pwd)/$(basename "$INPUT_A")
ABS_INPUT_A_DIR=$(dirname "$ABS_INPUT_A")
A_NAME=$(basename "$INPUT_A")
OUTPUT_BC_NAME="${A_NAME%.*}.bc"
UNZIP_DIR="${A_NAME}.unzip"

ORIG_DIR=$(pwd)
FULL_UNZIP_DIR="${ORIG_DIR}/${UNZIP_DIR}"

OUTPUT_BC_PATH="${ABS_INPUT_A_DIR}/${OUTPUT_BC_NAME}"

cleanup() {
    if [ -d "$FULL_UNZIP_DIR" ]; then
        echo "--> Cleaning up directory: $FULL_UNZIP_DIR"
        rm -rf "$FULL_UNZIP_DIR"
    fi
}
trap cleanup EXIT
echo "==> Processing: $A_NAME"

# 1. Create and enter the temporary directory
mkdir -p "$FULL_UNZIP_DIR"
cd "$FULL_UNZIP_DIR"

# 2. Extract the .a archive
ar x "$ABS_INPUT_A"

# 3. Identify and collect LLVM Bitcode files

BC_FILES=()

for f in *; do
    [ -f "$f" ] || continue

    # Method 1: Try to dump bitcode section from object file (.llvmbc or __llvm_bc)
    if ${LLVM_OBJCOPY} --dump-section .llvmbc="$f.extracted.bc" "$f" 2>/dev/null; then
        BC_FILES+=("$f.extracted.bc")
        continue
    elif ${LLVM_OBJCOPY} --dump-section __llvm_bc="$f.extracted.bc" "$f" 2>/dev/null; then
        BC_FILES+=("$f.extracted.bc")
        continue
    fi

    # Method 2: Check magic bytes for LLVM bitcode (BC\x04\x15)
    # BC file magic: 0x42 0x43 0x04 0x15
    if head -c 4 "$f" | od -An -tx1 | grep -q "42 43 04 15" 2>/dev/null; then
        BC_FILES+=("$f")
        continue
    fi

    # Method 3: Try to disassemble with llvm-dis as a final check
    if ${LLVM_DIS} "$f" -o /dev/null 2>/dev/null; then
        BC_FILES+=("$f")
    fi
done

TOTAL_BC=${#BC_FILES[@]}

if [ "$TOTAL_BC" -eq 0 ]; then
    echo "Error: No LLVM Bitcode found in $A_NAME! Make sure it was compiled with Clang and -flto."
    exit 1
fi

# 4. Link all Bitcode files into a single .bc
echo "--> Linking $TOTAL_BC bitcode files..."
${LLVM_LINK} "${BC_FILES[@]}" -o "$OUTPUT_BC_PATH"

# 5. Check if 'main' function exists
MAIN_FOUND="No"
if ${LLVM_NM} "$OUTPUT_BC_PATH" 2>/dev/null | grep -q -E ' T _?main$'; then
    MAIN_FOUND="Yes"
elif ${LLVM_DIS} -o - "$OUTPUT_BC_PATH" 2>/dev/null | grep -q -E 'define.*@main\('; then
    MAIN_FOUND="Yes"
fi

# 6. Report results
echo ""
echo "==================== Summary ===================="
echo "Extracted Bitcode Files : $TOTAL_BC"
echo "Contains 'main' Function: $MAIN_FOUND"
echo "Output File             : $OUTPUT_BC_PATH"
echo "================================================="