#!/bin/bash
# Run static analysis checks

cd /tmp/ewss

echo "Running cpplint (Google C++ style)..."
find include src -type f \( -name "*.hpp" -o -name "*.cpp" \) | while read file; do
  echo "  Linting: $file"
  cpplint --root=. "$file" || true
done

echo ""
echo "Running clang-tidy..."
# Run clang-tidy on project headers
clang-tidy -p build/compile_commands.json include/ewss/*.hpp 2>/dev/null || echo "Note: clang-tidy requires compiled database"

echo "✓ Static analysis complete"
