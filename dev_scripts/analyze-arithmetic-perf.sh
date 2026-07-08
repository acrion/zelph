#!/usr/bin/env bash
LOG_FILE=$(mktemp /tmp/zelph-mul.XXXXXX)
printf '.import arithmetic\n.log 9\n&12 * &34\n.quit\n' | build-release/bin/zelph >"${LOG_FILE}" 2>&1

echo "=== Final order frequency ==="
grep "Final order" "${LOG_FILE}" | sort | uniq -c | sort -rn

echo "=== Count optimized_snapshot=YES rel=mco subj= ==="
grep -c "optimized_snapshot=YES rel=mco subj=" "${LOG_FILE}"

echo "=== Count optimized_snapshot=YES rel=mco obj= ==="
grep -c "optimized_snapshot=YES rel=mco obj=" "${LOG_FILE}"

echo "=== Size distribution optimized_snapshot=YES rel=mco obj= ==="
grep "optimized_snapshot=YES rel=mco obj=" "${LOG_FILE}" | grep -o "size=[0-9]*" | sort -n | uniq -c
