#!/usr/bin/env bash

# Run the parallel mode and sort results to achieve a deterministic test result.
rm test-parallel.log
build-release/bin/zelph test.zph 2>&1 | sort >test-parallel.log 2>&1

# Create a copy with .parallel added at the beginning (to toggle parallel mode off)
echo ".parallel" > /tmp/test-single.zph
cat test.zph >> /tmp/test-single.zph

# Run the single mode
rm test-single.log
build-release/bin/zelph /tmp/test-single.zph >test-single.log 2>&1

# Check if either log file has uncommitted changes (working tree vs index/HEAD)
if git diff --quiet test-parallel.log test-single.log; then
    echo "Tests successful - no differences in log files"
    exit 0
else
    echo "Tests failed - differences detected in one or both log files"
    echo "Diff for test-parallel.log:"
    git diff test-parallel.log
    echo "Diff for test-single.log:"
    git diff test-single.log
    exit 1
fi
