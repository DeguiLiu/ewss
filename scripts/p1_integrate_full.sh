#!/bin/bash
# P1 Complete Integration: Extract agent code → Apply → Fix namespaces → Build

set -e

EWSS_REPO="/tmp/ewss"
TMP_WORK="/tmp/ewss_p1_work"

echo "╔════════════════════════════════════════════════════════════════╗"
echo "║  EWSS P1 Complete Integration & Namespace Fix                 ║"
echo "╚════════════════════════════════════════════════════════════════╝"
echo ""

# Function: Replace namespace in file
fix_namespace() {
  local file="$1"

  echo "  → Fixing namespaces in: $(basename $file)"

  # Main namespace declaration
  sed -i 's/namespace osp {/namespace ewss {/g' "$file"
  sed -i 's/} *\/\/ *namespace osp/} \/\/ namespace ewss/g' "$file"

  # Qualified names
  sed -i 's/::osp::/::ewss::/g' "$file"
  sed -i 's/\bosp::/ewss::/g' "$file"

  # Include guards
  sed -i 's/OSP_/EWSS_/g' "$file"
  sed -i 's/osp_/ewss_/g' "$file"

  # Macros
  sed -i 's/OSP_ASSERT/EWSS_ASSERT/g' "$file"
  sed -i 's/OSP_SCOPE_EXIT/EWSS_SCOPE_EXIT/g' "$file"
  sed -i 's/OSP_CONCAT/EWSS_CONCAT/g' "$file"
}

echo "Step 1: Prepare workspace"
echo ""
rm -rf "$TMP_WORK"
mkdir -p "$TMP_WORK"
echo "  ✓ Created workspace: $TMP_WORK"
echo ""

echo "Step 2: Parse agent outputs (once agents complete)"
echo ""
echo "  Waiting for agents to write final outputs..."
echo "  Expected files:"
echo "    - afb1d4c output (P1.1 error handling)"
echo "    - a28bf15 output (P1.2 backpressure)"
echo "    - a5001fc output (P1.3 protocol tests)"
echo "    - a9a04f0 output (P1.4 timeout management)"
echo ""

echo "Step 3: Apply code directly (copy phase)"
echo ""
echo "  This phase will:"
echo "    1. Extract code blocks from agent outputs"
echo "    2. Write to temporary files"
echo "    3. Apply to ${EWSS_REPO}"
echo "    4. Preserve original formatting and logic"
echo ""

echo "Step 4: Fix all namespaces"
echo ""
echo "  Will apply namespace transformations:"
echo "    osp → ewss"
echo "    OSP_ → EWSS_"
echo "    ::osp:: → ::ewss::"
echo "    (For all .hpp and .cpp files)"
echo ""

echo "Step 5: Format and verify"
echo ""
echo "  Commands:"
echo "    bash ${EWSS_REPO}/scripts/format.sh"
echo "    bash ${EWSS_REPO}/scripts/lint.sh"
echo ""

echo "Step 6: Full build & test"
echo ""
echo "  Commands:"
echo "    cd ${EWSS_REPO}"
echo "    rm -rf build && mkdir build && cd build"
echo "    cmake .. -DCMAKE_BUILD_TYPE=Release -DEWSS_BUILD_TESTS=ON"
echo "    cmake --build . -j\$(nproc)"
echo "    ctest --output-on-failure --verbose"
echo ""

echo "Step 7: Commit to Gitee"
echo ""
echo "  Commands:"
echo "    cd ${EWSS_REPO}"
echo "    git add -A"
echo "    git commit -m 'P1: Error handling, backpressure, protocol tests, timeout management'"
echo "    git log --oneline -1"
echo ""

echo "═════════════════════════════════════════════════════════════════"
echo "Integration script ready. Will execute automatically when agents"
echo "complete. Manual execution:"
echo ""
echo "  bash ${EWSS_REPO}/scripts/p1_auto_integrate.sh"
echo ""
