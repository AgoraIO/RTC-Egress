#!/bin/bash

# Search for processes matching either './bin/eg_worker' or './bin/egress' command
# Includes arguments like --config
matches=$(ps -eo pid,command | grep -E '\./bin/(eg_worker|egress)' | grep -v grep)

if [ -z "$matches" ]; then
  echo "❌ No eg_worker or egress process found."
  exit 0
fi

# Display matched processes
echo "✅ Found the following matching process(es):"
echo "$matches"
echo

# Ask the user to confirm before killing
read -p "⚠️ Do you want to kill ALL of these processes? (y/N): " confirm

# If not confirmed, exit safely
if [[ "$confirm" != "y" && "$confirm" != "Y" ]]; then
  echo "❎ Aborted by user."
  exit 0
fi

# Extract and kill each PID
echo "$matches" | awk '{print $1}' | while read pid; do
  echo "🔪 Killing PID $pid ..."
  kill -9 "$pid"
done

echo "✅ Done."
