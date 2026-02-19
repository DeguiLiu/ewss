#!/bin/bash
# Format code with clang-format

cd /tmp/ewss

echo "Running clang-format on all source files..."

# Find all .hpp and .cpp files
find include tests src examples -type f \( -name "*.hpp" -o -name "*.cpp" \) | while read file; do
  echo "  Formatting: $file"
  clang-format -i "$file" || echo "    Warning: clang-format failed on $file"
done

echo "✓ Code formatting complete"
