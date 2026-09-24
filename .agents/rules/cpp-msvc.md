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

3. **Clang-Format Standards**:
   - All modified and new C++ source and header files (`.cpp`, `.h`) in `OptiScaler/` and `tests/` must strictly comply with `.clang-format`.
   - Before committing any changes, format modified files with `clang-format -i` (or verify with `clang-format --dry-run --Werror`) to ensure the CI `clang-format Check` workflow passes cleanly without format violations.
   - Always preserve UTF-8 BOM (`\xef\xbb\xbf`) on files that use it when applying `clang-format`.
