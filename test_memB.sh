#!/bin/bash
# test_member_b.sh
# Tests specifically for Member B's functions: open, read, write, create, truncate
# Run this AFTER the full mini_unionfs binary is compiled and working.
# Usage: ./test_member_b.sh ./mini_unionfs

FUSE_BINARY="${1:-./mini_unionfs}"
TEST_DIR="./memberb_test"
LOWER="$TEST_DIR/lower"
UPPER="$TEST_DIR/upper"
MNT="$TEST_DIR/mnt"

GREEN='\033[0;32m'
RED='\033[0;31m'
YELLOW='\033[1;33m'
NC='\033[0m'

PASS=0
FAIL=0

check() {
    local label="$1"
    local result="$2"
    if [ "$result" = "1" ]; then
        echo -e "  ${GREEN}PASS${NC}: $label"
        ((PASS++))
    else
        echo -e "  ${RED}FAIL${NC}: $label"
        ((FAIL++))
    fi
}

echo -e "${YELLOW}=== Member B Test Suite: open/read/write/create/truncate ===${NC}"

# ---- Setup ----
rm -rf "$TEST_DIR"
mkdir -p "$LOWER" "$UPPER" "$MNT"

# Seed lower_dir with test files
echo "original content" > "$LOWER/cow_test.txt"
echo "read only file"   > "$LOWER/read_test.txt"
echo "to be truncated"  > "$LOWER/trunc_test.txt"

# Mount
"$FUSE_BINARY" "$LOWER" "$UPPER" "$MNT"
if [ $? -ne 0 ]; then
    echo -e "${RED}Mount failed. Is the binary built? Is FUSE installed?${NC}"
    rm -rf "$TEST_DIR"
    exit 1
fi
sleep 1
echo ""

# ---- Test 1: Read a file from lower_dir through the mount ----
echo "--- Test 1: Reading lower_dir file through mount ---"
content=$(cat "$MNT/read_test.txt" 2>/dev/null)
check "read_test.txt visible through mount" \
    $([ "$content" = "read only file" ] && echo 1 || echo 0)

# ---- Test 2: CoW — modifying a lower-only file copies it to upper ----
echo ""
echo "--- Test 2: Copy-on-Write ---"
echo "appended line" >> "$MNT/cow_test.txt"
sleep 0.2

check "modified content visible in mount" \
    $(grep -q "appended line" "$MNT/cow_test.txt" 2>/dev/null && echo 1 || echo 0)

check "copy appeared in upper_dir" \
    $([ -f "$UPPER/cow_test.txt" ] && echo 1 || echo 0)

check "upper_dir copy has modified content" \
    $(grep -q "appended line" "$UPPER/cow_test.txt" 2>/dev/null && echo 1 || echo 0)

check "lower_dir file is untouched (original content only)" \
    $([ "$(cat "$LOWER/cow_test.txt")" = "original content" ] && echo 1 || echo 0)

check "lower_dir file does NOT have appended line" \
    $(grep -qv "appended line" "$LOWER/cow_test.txt" 2>/dev/null && echo 1 || echo 0)

# ---- Test 3: Create a new file through the mount ----
echo ""
echo "--- Test 3: create() — new file goes to upper_dir only ---"
echo "brand new file" > "$MNT/new_file.txt"
sleep 0.2

check "new file visible through mount" \
    $([ -f "$MNT/new_file.txt" ] && echo 1 || echo 0)

check "new file created in upper_dir" \
    $([ -f "$UPPER/new_file.txt" ] && echo 1 || echo 0)

check "new file NOT in lower_dir" \
    $([ ! -f "$LOWER/new_file.txt" ] && echo 1 || echo 0)

check "new file has correct content" \
    $([ "$(cat "$UPPER/new_file.txt")" = "brand new file" ] && echo 1 || echo 0)

# ---- Test 4: Write to an already-upper file (no CoW needed) ----
echo ""
echo "--- Test 4: Write to upper_dir file (no CoW needed) ---"
echo "upper only content" > "$UPPER/upper_only.txt"
sleep 0.2
echo "written via mount" >> "$MNT/upper_only.txt"
sleep 0.2

check "write to upper-only file works" \
    $(grep -q "written via mount" "$MNT/upper_only.txt" 2>/dev/null && echo 1 || echo 0)

# ---- Test 5: Truncate triggers CoW on lower-only file ----
echo ""
echo "--- Test 5: Truncate a lower_dir file (CoW before truncate) ---"
truncate -s 5 "$MNT/trunc_test.txt" 2>/dev/null
sleep 0.2

check "truncated copy appeared in upper_dir" \
    $([ -f "$UPPER/trunc_test.txt" ] && echo 1 || echo 0)

upper_size=$(stat -c%s "$UPPER/trunc_test.txt" 2>/dev/null)
check "upper_dir copy is truncated to 5 bytes" \
    $([ "$upper_size" = "5" ] && echo 1 || echo 0)

lower_size=$(stat -c%s "$LOWER/trunc_test.txt" 2>/dev/null)
check "lower_dir file is unchanged" \
    $([ "$lower_size" != "5" ] && echo 1 || echo 0)

# ---- Teardown ----
echo ""
fusermount -u "$MNT" 2>/dev/null || umount "$MNT" 2>/dev/null
rm -rf "$TEST_DIR"

echo ""
echo -e "${YELLOW}=== Results: ${GREEN}${PASS} passed${NC}, ${RED}${FAIL} failed${NC} ==="
if [ "$FAIL" -eq 0 ]; then
    echo -e "${GREEN}All Member B tests passed! Ready to push.${NC}"
else
    echo -e "${RED}Some tests failed. Debug hints:${NC}"
    echo "  - Test 2 fail → CoW in unionfs_open() is broken"
    echo "  - Test 3 fail → unionfs_create() not writing to upper_dir"
    echo "  - Test 5 fail → unionfs_truncate() not triggering CoW"
fi
