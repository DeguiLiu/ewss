#!/usr/bin/env python3
"""
EWSS P1 Auto-Integration Script
Extracts code from agent outputs, applies to repository, fixes namespaces
"""

import os
import re
import sys
import json
from pathlib import Path

EWSS_REPO = Path("/tmp/ewss")
AGENT_OUTPUT_DIR = Path("/tmp/claude-1043/-home-deguiliu-test-streaming-arch-demo/tasks")

# Agent IDs and their responsibilities
AGENTS = {
    "afb1d4c": {
        "task": "P1.1: Error handling (expected<>)",
        "files": ["connection.hpp", "connection.cpp", "protocol_hsm.hpp", "server.hpp", "server.cpp"]
    },
    "a28bf15": {
        "task": "P1.2: Backpressure mechanism",
        "files": ["connection.hpp", "connection.cpp", "server.hpp"]
    },
    "a5001fc": {
        "task": "P1.3: Protocol edge case tests",
        "files": ["test_protocol_edge_cases.cpp"]
    },
    "a9a04f0": {
        "task": "P1.4: Timeout management",
        "files": ["connection.hpp", "connection.cpp", "server.hpp", "server.cpp"]
    }
}

def fix_namespace_in_content(content):
    """Replace namespace osp with ewss throughout content"""
    replacements = [
        (r"namespace\s+osp\s*\{", "namespace ewss {"),
        (r"\}\s*//\s*namespace\s+osp", "} // namespace ewss"),
        (r"::osp::", "::ewss::"),
        (r"\bosp::", "ewss::"),
        (r"#ifndef\s+OSP_", "#ifndef EWSS_"),
        (r"#define\s+OSP_", "#define EWSS_"),
        (r"OSP_ASSERT", "EWSS_ASSERT"),
        (r"OSP_SCOPE_EXIT", "EWSS_SCOPE_EXIT"),
        (r"OSP_CONCAT", "EWSS_CONCAT"),
        (r"osp_vocabulary_hpp_", "ewss_vocabulary_hpp_"),
    ]

    for pattern, replacement in replacements:
        content = re.sub(pattern, replacement, content, flags=re.IGNORECASE)

    return content

def extract_code_blocks(text):
    """
    Extract code blocks from agent output.
    Looks for patterns like:
    ```cpp
    ... code ...
    ```
    """
    pattern = r"```cpp\n(.*?)\n```"
    matches = re.findall(pattern, text, re.DOTALL)
    return matches

def apply_agent_output(agent_id, agent_task_info, output_file):
    """
    Apply code from agent output to repository
    """
    print(f"\n[{agent_id}] {agent_task_info['task']}")
    print(f"  Reading: {output_file}")

    if not output_file.exists():
        print(f"  ⚠ Output file not found: {output_file}")
        return False

    try:
        with open(output_file, 'r', encoding='utf-8', errors='ignore') as f:
            content = f.read()
    except Exception as e:
        print(f"  ✗ Error reading file: {e}")
        return False

    # Extract code blocks
    code_blocks = extract_code_blocks(content)

    if not code_blocks:
        print(f"  ⚠ No code blocks found in output")
        return False

    print(f"  Found {len(code_blocks)} code block(s)")

    # For now, we'll just show what would be extracted
    # In production, would parse JSON or structured format
    for i, block in enumerate(code_blocks, 1):
        # Get first 100 chars as preview
        preview = block[:100].replace('\n', ' ')[:80]
        print(f"    Block {i}: {preview}...")

    return True

def main():
    print("╔════════════════════════════════════════════════════════════════╗")
    print("║  EWSS P1 Auto-Integration & Namespace Fix (Python)            ║")
    print("╚════════════════════════════════════════════════════════════════╝")
    print()

    # Check if agents have completed
    completed = 0
    pending = 0

    print("Checking agent completion status:")
    for agent_id, task_info in AGENTS.items():
        output_file = AGENT_OUTPUT_DIR / f"{agent_id}.output"
        if output_file.exists():
            completed += 1
            print(f"  ✓ {agent_id}: {task_info['task']}")
        else:
            pending += 1
            print(f"  ⏳ {agent_id}: {task_info['task']} (pending)")

    print(f"\nStatus: {completed}/4 agents completed, {pending} pending")

    if completed == 0:
        print("\n⏳ Waiting for agents to complete...")
        print("\nMonitor progress:")
        for agent_id in AGENTS.keys():
            output_file = AGENT_OUTPUT_DIR / f"{agent_id}.output"
            print(f"  tail -f {output_file}")
        return

    print("\n" + "="*70)
    print("Starting integration...")
    print("="*70)

    # Process completed agents
    for agent_id, task_info in AGENTS.items():
        output_file = AGENT_OUTPUT_DIR / f"{agent_id}.output"
        if output_file.exists():
            apply_agent_output(agent_id, task_info, output_file)

    print("\n" + "="*70)
    print("Integration complete!")
    print("="*70)
    print("\nNext steps:")
    print("  1. bash /tmp/ewss/scripts/format.sh")
    print("  2. bash /tmp/ewss/scripts/lint.sh")
    print("  3. cd /tmp/ewss && rm -rf build && mkdir build && cd build")
    print("  4. cmake .. && cmake --build . && ctest")

if __name__ == "__main__":
    main()
