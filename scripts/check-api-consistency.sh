#!/bin/bash
#
# QuantumOS API Consistency Checker
#
# Validates that function calls in .c files match declarations in .h files.
# This prevents "phantom API" drift where code calls non-existent functions.
#
# Usage: ./scripts/check-api-consistency.sh [--verbose] [--fix-suggestions]
#

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$SCRIPT_DIR")"
KERNEL_DIR="$PROJECT_ROOT/kernel"

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

VERBOSE=0
FIX_SUGGESTIONS=0
ERRORS_FOUND=0
WARNINGS_FOUND=0

# Parse arguments
for arg in "$@"; do
    case $arg in
        --verbose|-v)
            VERBOSE=1
            ;;
        --fix-suggestions|-f)
            FIX_SUGGESTIONS=1
            ;;
        --help|-h)
            echo "Usage: $0 [--verbose] [--fix-suggestions]"
            echo ""
            echo "Options:"
            echo "  --verbose, -v         Show detailed output"
            echo "  --fix-suggestions, -f Show suggested fixes for errors"
            echo "  --help, -h            Show this help"
            exit 0
            ;;
    esac
done

echo -e "${BLUE}=== QuantumOS API Consistency Checker ===${NC}"
echo ""

# Build list of declared functions from headers
declare -A DECLARED_FUNCTIONS
declare -A FUNCTION_HEADERS

echo -e "${BLUE}[1/4] Scanning header files for function declarations...${NC}"

for header in $(find "$KERNEL_DIR/include" -name "*.h" 2>/dev/null); do
    header_name=$(basename "$header")

    # Extract function declarations (simplified pattern matching)
    # Matches: return_type function_name(params);
    while IFS= read -r line; do
        # Skip comments and preprocessor
        [[ "$line" =~ ^[[:space:]]*/\* ]] && continue
        [[ "$line" =~ ^[[:space:]]*\* ]] && continue
        [[ "$line" =~ ^[[:space:]]*# ]] && continue
        [[ "$line" =~ ^[[:space:]]*// ]] && continue

        # Strip string literals so text like boot_log("Phi (x1000): ")
        # is not parsed as a call to a function named Phi.
        line="${line//\\\"/}"
        while [[ "$line" =~ \"[^\"]*\" ]]; do
            line="${line/"${BASH_REMATCH[0]}"/}"
        done

        # Match function declarations
        if [[ "$line" =~ ^[a-zA-Z_][a-zA-Z0-9_*[:space:]]+[[:space:]*]+([a-zA-Z_][a-zA-Z0-9_]*)[[:space:]]*\( ]]; then
            func_name="${BASH_REMATCH[1]}"
            # Skip common non-functions
            [[ "$func_name" == "if" ]] && continue
            [[ "$func_name" == "while" ]] && continue
            [[ "$func_name" == "for" ]] && continue
            [[ "$func_name" == "switch" ]] && continue
            [[ "$func_name" == "return" ]] && continue
            [[ "$func_name" == "sizeof" ]] && continue
            [[ "$func_name" == "typedef" ]] && continue

            DECLARED_FUNCTIONS["$func_name"]=1
            FUNCTION_HEADERS["$func_name"]="$header_name"

            if [ $VERBOSE -eq 1 ]; then
                echo "  Found: $func_name in $header_name"
            fi
        fi
    done < "$header"
done

echo -e "  Found ${GREEN}${#DECLARED_FUNCTIONS[@]}${NC} function declarations"
echo ""

# Scan source files for function calls
echo -e "${BLUE}[2/4] Scanning source files for function calls...${NC}"

declare -A CALLED_FUNCTIONS
declare -A CALL_LOCATIONS

for source in $(find "$KERNEL_DIR/src" -name "*.c" 2>/dev/null); do
    source_name=$(basename "$source")
    line_num=0
    in_block_comment=0

    while IFS= read -r line; do
        line_num=$((line_num+1))

        # Track multi-line /* ... */ blocks (continuation lines may not
        # start with a comment marker).
        if [ "$in_block_comment" -eq 1 ]; then
            if [[ "$line" == *'*/'* ]]; then
                in_block_comment=0
                line="${line#*\*/}"
            else
                continue
            fi
        fi

        # Skip comments (an opener without a closer starts a block)
        if [[ "$line" =~ ^[[:space:]]*/\* ]]; then
            [[ "$line" != *'*/'* ]] && in_block_comment=1
            continue
        fi
        [[ "$line" =~ ^[[:space:]]*\* ]] && continue
        [[ "$line" =~ ^[[:space:]]*// ]] && continue

        # Skip preprocessor lines (#define/#include/#pragma ...)
        [[ "$line" =~ ^[[:space:]]*# ]] && continue

        # Strip string literals so text like boot_log("Phi (x1000): ")
        # is not parsed as a call to a function named Phi.
        line="${line//\\\"/}"
        while [[ "$line" =~ \"[^\"]*\" ]]; do
            line="${line/"${BASH_REMATCH[0]}"/}"
        done

        # Strip __attribute__((...)) clauses — consuming their inner parens
        # otherwise leaves the preceding identifier adjacent to '(' and it
        # gets miscounted as a call (e.g. next_pid __attribute__((unused))).
        while [[ "$line" =~ __attribute__[[:space:]]*\(\([^\)]*\)\) ]]; do
            line="${line/"${BASH_REMATCH[0]}"/}"
        done

        # Strip inline /* ... */ blocks and trailing // comments so
        # commented-out calls on code lines are not counted.
        while [[ "$line" == *'/*'*'*/'* ]]; do
            line="${line%%/\**}${line##*\*/}"
        done
        line="${line%%//*}"
        if [[ "$line" == *'/*'* ]]; then
            line="${line%%/\**}"
            in_block_comment=1
        fi

        # Calls through a struct member (ops.fn(), ops->fn()) go through a
        # function pointer the compiler type-checks against its struct; they
        # name no global symbol, so they cannot be phantom APIs. Drop the
        # member name and keep the paren.
        while [[ "$line" =~ (\.|->)[[:space:]]*[a-zA-Z_][a-zA-Z0-9_]*[[:space:]]*\( ]]; do
            line="${line/"${BASH_REMATCH[0]}"/ (}"
        done

        # Find function calls: identifier followed by (
        # This is a simplified check - production would use proper parsing
        while [[ "$line" =~ ([a-zA-Z_][a-zA-Z0-9_]*)[[:space:]]*\( ]]; do
            func_name="${BASH_REMATCH[1]}"

            # Skip common non-functions and keywords (_Static_assert is
            # the C11 compile-time assert keyword, not a function call;
            # `else`/`do`/`goto` can be left adjacent to a '(' when the
            # naive removal above strips the preceding `if (`).
            case "$func_name" in
                if|else|do|goto|while|for|switch|return|sizeof|typedef|struct|enum|union|static|extern|const|volatile|inline|__attribute__|ALIGNED|PACKED|_Static_assert)
                    ;;
                *)
                    CALLED_FUNCTIONS["$func_name"]=1
                    if [ -z "${CALL_LOCATIONS[$func_name]}" ]; then
                        CALL_LOCATIONS["$func_name"]="$source_name:$line_num"
                    fi
                    ;;
            esac

            # Remove the match to find more in the same line
            line="${line/${BASH_REMATCH[0]}/}"
        done
    done < "$source"
done

echo -e "  Found ${GREEN}${#CALLED_FUNCTIONS[@]}${NC} unique function calls"
echo ""

# Check for undeclared functions (potential phantom APIs)
echo -e "${BLUE}[3/4] Checking for undeclared function calls...${NC}"

# Known external/builtin functions to ignore
KNOWN_EXTERNALS="memset memcpy strlen strncpy printf snprintf boot_log boot_panic early_console_write early_console_init __builtin_offsetof offsetof"

for func in "${!CALLED_FUNCTIONS[@]}"; do
    # Skip if declared
    [ "${DECLARED_FUNCTIONS[$func]}" == "1" ] && continue

    # Skip known externals
    skip=0
    for ext in $KNOWN_EXTERNALS; do
        [ "$func" == "$ext" ] && skip=1 && break
    done
    [ $skip -eq 1 ] && continue

    # Skip if it looks like a local/static function (defined in same file)
    # This is a heuristic - might have false positives
    location="${CALL_LOCATIONS[$func]}"
    source_file="${location%%:*}"

    # Check if function is defined in the same source file (the file may
    # live in any subdirectory of src/ — locations record the basename).
    if [ -n "$source_file" ]; then
        found_def=0
        while IFS= read -r full_path; do
            if grep -q "^[a-zA-Z_][a-zA-Z0-9_* ]*${func}[[:space:]]*(" "$full_path" 2>/dev/null ||                grep -qE "^[a-zA-Z_][a-zA-Z0-9_* ]*[* ]${func}[[:space:]]*(=|;)" "$full_path" 2>/dev/null; then
                found_def=1
                break
            fi
        done < <(find "$KERNEL_DIR/src" -name "$source_file" -type f 2>/dev/null)
        [ "$found_def" -eq 1 ] && continue
    fi

    # Function-like macros count as definitions: #define NAME( — in the
    # calling file or any header.
    if [ -n "$source_file" ]; then
        macro_def=0
        while IFS= read -r full_path; do
            if grep -q "^#[[:space:]]*define[[:space:]]\+${func}(" "$full_path" 2>/dev/null; then
                macro_def=1
                break
            fi
        done < <(find "$KERNEL_DIR/src" -name "$source_file" -type f 2>/dev/null)
        [ "$macro_def" -eq 1 ] && continue
    fi
    if grep -rq "^#[[:space:]]*define[[:space:]]\+${func}(" "$KERNEL_DIR/include" 2>/dev/null; then
        continue
    fi

    # This is a potential phantom API call
    echo -e "${RED}ERROR: Undeclared function call: ${func}${NC}"
    echo -e "  Location: ${CALL_LOCATIONS[$func]}"

    if [ $FIX_SUGGESTIONS -eq 1 ]; then
        # Try to find similar function names
        echo -e "  ${YELLOW}Possible matches:${NC}"
        for declared in "${!DECLARED_FUNCTIONS[@]}"; do
            # Simple similarity: same prefix
            prefix="${func:0:4}"
            if [[ "$declared" == *"$prefix"* ]]; then
                echo -e "    - $declared (from ${FUNCTION_HEADERS[$declared]})"
            fi
        done
    fi

    ERRORS_FOUND=$((ERRORS_FOUND+1))
    echo ""
done

if [ $ERRORS_FOUND -eq 0 ]; then
    echo -e "  ${GREEN}No undeclared function calls found${NC}"
fi
echo ""

# Check for common drift patterns
echo -e "${BLUE}[4/4] Checking for known drift patterns...${NC}"

# Pattern 1: Old IPC API that was replaced
OLD_IPC_PATTERNS="ipc_create_queue ipc_destroy_queue ipc_queue_create ipc_queue_destroy"
for pattern in $OLD_IPC_PATTERNS; do
    matches=$(grep -rn "$pattern" "$KERNEL_DIR/src" 2>/dev/null || true)
    if [ -n "$matches" ]; then
        echo -e "${RED}ERROR: Found deprecated IPC API usage: $pattern${NC}"
        echo "$matches" | while read -r match; do
            echo -e "  $match"
        done
        echo -e "  ${YELLOW}Use ipc_process_init() / ipc_process_cleanup() instead${NC}"
        ERRORS_FOUND=$((ERRORS_FOUND+1))
        echo ""
    fi
done

# Pattern 2: Check docs match code
DOC_FILES=$(find "$PROJECT_ROOT/docs" -name "*.md" 2>/dev/null || true)
for doc in $DOC_FILES; do
    for pattern in $OLD_IPC_PATTERNS; do
        if grep -q "$pattern" "$doc" 2>/dev/null; then
            echo -e "${YELLOW}WARNING: Documentation contains outdated API: $pattern${NC}"
            echo -e "  File: $doc"
            WARNINGS_FOUND=$((WARNINGS_FOUND+1))
        fi
    done
done

# Pattern 3: Ring-3 text-dump buffers must cover their kernel rings WHOLE.
# There is no shared header across the ring boundary, so these constants can
# silently drift: AUDIT_TEXT_MAX was left at 12288 (the 128-entry era) when the
# audit ring doubled to 256, and every dump past 12KB truncated MID-LINE — the
# MCP gate's late REAP probes read a ledger that simply ended before the rows
# they poll for. Recompute the kernel's sizes here and fail on any mismatch.
AUDIT_ENTRIES=$(grep -oP '#define AUDIT_LOG_ENTRIES \K[0-9]+' "$KERNEL_DIR/include/kernel/audit.h" || echo 0)
AUDIT_LINE=$(grep -oP '#define AUDIT_LINE_MAX \K[0-9]+' "$KERNEL_DIR/include/kernel/audit.h" || echo 0)
USER_AUDIT_MAX=$(grep -oP '#define AUDIT_TEXT_MAX \K[0-9]+' "$PROJECT_ROOT/user/usys.h" || echo 0)
KERNEL_AUDIT_MAX=$((AUDIT_ENTRIES * AUDIT_LINE))
if [ "$KERNEL_AUDIT_MAX" -eq 0 ] || [ "$USER_AUDIT_MAX" -lt "$KERNEL_AUDIT_MAX" ]; then
    echo -e "${RED}ERROR: user AUDIT_TEXT_MAX ($USER_AUDIT_MAX) < kernel AUDIT_MAX_BYTES ($KERNEL_AUDIT_MAX = ${AUDIT_ENTRIES}*${AUDIT_LINE}) — audit dumps will truncate mid-line${NC}"
    ERRORS_FOUND=$((ERRORS_FOUND+1))
fi
KERNEL_MANIFEST_MAX=$(grep -oP '#define MANIFEST_TEXT_MAX \K[0-9]+' "$KERNEL_DIR/include/kernel/manifest.h" || echo 0)
USER_MANIFEST_MAX=$(grep -oP '#define MANIFEST_TEXT_MAX \K[0-9]+' "$PROJECT_ROOT/user/usys.h" || echo 0)
if [ "$KERNEL_MANIFEST_MAX" -eq 0 ] || [ "$USER_MANIFEST_MAX" -lt "$KERNEL_MANIFEST_MAX" ]; then
    echo -e "${RED}ERROR: user MANIFEST_TEXT_MAX ($USER_MANIFEST_MAX) < kernel MANIFEST_TEXT_MAX ($KERNEL_MANIFEST_MAX) — manifest dumps will truncate${NC}"
    ERRORS_FOUND=$((ERRORS_FOUND+1))
fi

echo ""
echo -e "${BLUE}=== Summary ===${NC}"
if [ $ERRORS_FOUND -eq 0 ] && [ $WARNINGS_FOUND -eq 0 ]; then
    echo -e "${GREEN}All checks passed!${NC}"
    exit 0
else
    if [ $ERRORS_FOUND -gt 0 ]; then
        echo -e "${RED}Errors: $ERRORS_FOUND${NC}"
    fi
    if [ $WARNINGS_FOUND -gt 0 ]; then
        echo -e "${YELLOW}Warnings: $WARNINGS_FOUND${NC}"
    fi
    echo ""
    echo "Please fix the errors before submitting a PR."
    exit 1
fi
