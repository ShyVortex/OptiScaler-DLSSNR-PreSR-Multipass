---
trigger: always_on
description: C++ and MSVC compilation standards for OptiScaler
---

# C++ & MSVC Compiler Guidelines

1. **C++20 `std::format` on Enums**:
   - MSVC's `<format>` implementation does not provide an implicit formatter for enum types when used with formatting specifiers like `{:x}`.
   - **Always** explicitly cast enums to an integer type (e.g. `static_cast<uint32_t>(enumValue)`) before passing them to `std::format`.

2. **File Encodings & UTF-8 BOM**:
   - Visual Studio project files (`.vcxproj`, `.vcxproj.filters`) and C++ source/header files in this repo use UTF-8 with BOM (`\xef\xbb\xbf`).
   - Always preserve or restore the BOM when editing these files to prevent unnecessary git diffs and ensure clean MSVC parsing.
