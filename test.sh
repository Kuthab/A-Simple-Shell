#!/bin/bash
# test.sh — automated tests for mysh
# Run: make test   (or: bash test.sh)

MYSH=./mysh
PASS=0
FAIL=0
TOTAL=0

pass() { PASS=$((PASS+1)); TOTAL=$((TOTAL+1)); echo "  PASS: $1"; }
fail() { FAIL=$((FAIL+1)); TOTAL=$((TOTAL+1)); echo "  FAIL: $1"; }

check() {
    # $1 = test name, $2 = expected output, $3 = actual output
    if [ "$2" = "$3" ]; then pass "$1"; else
        fail "$1"
        echo "    expected: $(echo "$2" | head -3)"
        echo "    got:      $(echo "$3" | head -3)"
    fi
}

# Create temp directory for test files
TMPDIR=$(mktemp -d)
trap "rm -rf $TMPDIR" EXIT

# ------------------------------------------------------------------
echo "--- Basic commands (batch mode) ---"
# ------------------------------------------------------------------

OUT=$(echo "echo hello" | $MYSH)
check "echo hello" "hello" "$OUT"

OUT=$(echo "echo hello world" | $MYSH)
check "echo hello world" "hello world" "$OUT"

OUT=$(echo "" | $MYSH)
check "empty command" "" "$OUT"

OUT=$(echo "# this is a comment" | $MYSH)
check "comment-only line" "" "$OUT"

OUT=$(echo "echo hello # comment" | $MYSH)
check "command with trailing comment" "hello" "$OUT"

# ------------------------------------------------------------------
echo "--- Built-in: pwd ---"
# ------------------------------------------------------------------

EXPECTED=$(pwd)
OUT=$(echo "pwd" | $MYSH)
check "pwd" "$EXPECTED" "$OUT"

# ------------------------------------------------------------------
echo "--- Built-in: cd ---"
# ------------------------------------------------------------------

OUT=$(echo -e "cd /tmp\npwd" | $MYSH)
check "cd /tmp then pwd" "/tmp" "$OUT"

OUT=$(echo "cd" | $MYSH)
check "cd with no args (home)" "" "$OUT"

OUT=$(printf "cd /tmp\ncd ..\npwd\n" | $MYSH)
check "cd /tmp, cd .., pwd" "/" "$OUT"

OUT=$(echo "cd /nonexistent_dir_12345" | $MYSH 2>/dev/null)
check "cd to nonexistent dir (no output)" "" "$OUT"

# ------------------------------------------------------------------
echo "--- Built-in: which ---"
# ------------------------------------------------------------------

OUT=$(echo "which ls" | $MYSH)
# ls should be in /usr/bin/ls or /bin/ls
if echo "$OUT" | grep -qE "^/(usr/)?bin/ls$"; then pass "which ls"
else fail "which ls (got: $OUT)"; fi

OUT=$(echo "which cd" | $MYSH 2>/dev/null)
check "which cd (builtin, should fail)" "" "$OUT"

OUT=$(echo "which nonexistent_program_xyz" | $MYSH 2>/dev/null)
check "which nonexistent" "" "$OUT"

# ------------------------------------------------------------------
echo "--- Built-in: exit ---"
# ------------------------------------------------------------------

OUT=$(echo -e "exit\necho should not print" | $MYSH)
check "exit stops execution" "" "$OUT"

# ------------------------------------------------------------------
echo "--- Output redirection ---"
# ------------------------------------------------------------------

REDIR_OUT="$TMPDIR/redir_out.txt"
echo "echo redirected > $REDIR_OUT" | $MYSH
OUT=$(cat "$REDIR_OUT" 2>/dev/null)
check "output redirection" "redirected" "$OUT"

# Check file permissions (should be 0640)
PERMS=$(stat -c "%a" "$REDIR_OUT" 2>/dev/null || stat -f "%Lp" "$REDIR_OUT" 2>/dev/null)
check "output redirection permissions" "640" "$PERMS"

# ------------------------------------------------------------------
echo "--- Input redirection ---"
# ------------------------------------------------------------------

echo "hello from file" > "$TMPDIR/input.txt"
OUT=$(echo "cat < $TMPDIR/input.txt" | $MYSH)
check "input redirection" "hello from file" "$OUT"

# ------------------------------------------------------------------
echo "--- Both redirections ---"
# ------------------------------------------------------------------

echo "both test" > "$TMPDIR/both_in.txt"
echo "cat < $TMPDIR/both_in.txt > $TMPDIR/both_out.txt" | $MYSH
OUT=$(cat "$TMPDIR/both_out.txt" 2>/dev/null)
check "input + output redirection" "both test" "$OUT"

# Test reverse order of redirections
echo "cat > $TMPDIR/both_out2.txt < $TMPDIR/both_in.txt" | $MYSH
OUT=$(cat "$TMPDIR/both_out2.txt" 2>/dev/null)
check "output then input redirection" "both test" "$OUT"

# ------------------------------------------------------------------
echo "--- Pipes ---"
# ------------------------------------------------------------------

OUT=$(echo "echo hello | cat" | $MYSH)
check "simple pipe" "hello" "$OUT"

OUT=$(echo "echo hello world | wc -w" | $MYSH)
OUT=$(echo "$OUT" | tr -d ' ')
check "echo | wc -w" "2" "$OUT"

OUT=$(echo "echo abc | cat | cat | cat" | $MYSH)
check "multi-pipe" "abc" "$OUT"

# ------------------------------------------------------------------
echo "--- Wildcards ---"
# ------------------------------------------------------------------

mkdir -p "$TMPDIR/wc_test"
touch "$TMPDIR/wc_test/foo.txt" "$TMPDIR/wc_test/bar.txt" "$TMPDIR/wc_test/baz.c"

OUT=$(echo "echo $TMPDIR/wc_test/*.txt" | $MYSH)
# Should contain both .txt files (order may vary)
if echo "$OUT" | grep -q "foo.txt" && echo "$OUT" | grep -q "bar.txt"; then
    pass "wildcard *.txt"
else
    fail "wildcard *.txt (got: $OUT)"
fi

# Wildcard with no matches should pass through unchanged
OUT=$(echo "echo $TMPDIR/wc_test/*.xyz" | $MYSH)
check "wildcard no match (passthrough)" "$TMPDIR/wc_test/*.xyz" "$OUT"

# Hidden files not matched by leading *
touch "$TMPDIR/wc_test/.hidden"
OUT=$(echo "echo $TMPDIR/wc_test/*" | $MYSH)
if echo "$OUT" | grep -q ".hidden"; then
    fail "wildcard should not match hidden files"
else
    pass "wildcard skips hidden files"
fi

# ------------------------------------------------------------------
echo "--- Batch mode: stdin is /dev/null ---"
# ------------------------------------------------------------------

# In batch mode, child processes should get /dev/null as stdin
OUT=$(echo "cat" | $MYSH)
check "batch mode stdin /dev/null" "" "$OUT"

# ------------------------------------------------------------------
echo "--- Syntax errors ---"
# ------------------------------------------------------------------

# These should fail but not crash mysh
OUT=$(echo -e "< <\necho after_error" | $MYSH 2>/dev/null)
check "syntax error recovery" "after_error" "$OUT"

OUT=$(echo -e "|\necho after_pipe_error" | $MYSH 2>/dev/null)
check "leading pipe error recovery" "after_pipe_error" "$OUT"

# ------------------------------------------------------------------
echo "--- Batch mode from file ---"
# ------------------------------------------------------------------

echo "echo from_file" > "$TMPDIR/script.sh"
OUT=$($MYSH "$TMPDIR/script.sh")
check "batch mode from file" "from_file" "$OUT"

# Script with multiple commands
printf "echo line1\necho line2\n" > "$TMPDIR/multi.sh"
OUT=$($MYSH "$TMPDIR/multi.sh")
EXPECTED=$(printf "line1\nline2")
check "batch mode multi-line script" "$EXPECTED" "$OUT"

# ------------------------------------------------------------------
echo "--- Conditionals (then/else) ---"
# ------------------------------------------------------------------

# then after successful command
OUT=$(printf "echo ok\nthen echo yes\n" | $MYSH)
EXPECTED=$(printf "ok\nyes")
check "then after success" "$EXPECTED" "$OUT"

# then after failed command (should skip)
OUT=$(printf "ls /nonexistent_dir_xyz 2>/dev/null\nthen echo should_not_print\n" | $MYSH 2>/dev/null)
check "then after failure (skip)" "" "$OUT"

# else after failed command
OUT=$(printf "ls /nonexistent_dir_xyz 2>/dev/null\nelse echo fallback\n" | $MYSH 2>/dev/null)
check "else after failure" "fallback" "$OUT"

# else after successful command (should skip)
OUT=$(printf "echo ok\nelse echo should_not_print\n" | $MYSH)
check "else after success (skip)" "ok" "$OUT"

# chained: then after then
OUT=$(printf "echo ok\nthen echo step2\nthen echo step3\n" | $MYSH)
EXPECTED=$(printf "ok\nstep2\nstep3")
check "then chain" "$EXPECTED" "$OUT"

# else then chain: fail, else runs, then runs after else success
OUT=$(printf "ls /nonexistent_dir_xyz 2>/dev/null\nelse echo recovered\nthen echo continued\n" | $MYSH 2>/dev/null)
EXPECTED=$(printf "recovered\ncontinued")
check "else then chain" "$EXPECTED" "$OUT"

# ------------------------------------------------------------------
echo "--- Pipeline with exit ---"
# ------------------------------------------------------------------

OUT=$(echo "echo pipeline_exit | cat" | $MYSH)
check "pipe before exit" "pipeline_exit" "$OUT"

# ------------------------------------------------------------------
echo "--- Launch by path ---"
# ------------------------------------------------------------------

OUT=$(echo "/bin/echo path_test" | $MYSH)
check "launch by absolute path" "path_test" "$OUT"

# ------------------------------------------------------------------
echo "--- Launch by name ---"
# ------------------------------------------------------------------

OUT=$(echo "echo name_test" | $MYSH)
check "launch by bare name" "name_test" "$OUT"

# ------------------------------------------------------------------
echo "--- EOF terminates shell (die) ---"
# ------------------------------------------------------------------

OUT=$(printf "echo before_eof" | $MYSH)
check "EOF terminates (no exit needed)" "before_eof" "$OUT"

# ------------------------------------------------------------------
echo ""
echo "=== Results: $PASS passed, $FAIL failed, $TOTAL total ==="

if [ $FAIL -gt 0 ]; then exit 1; fi
exit 0
