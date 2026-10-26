---
trigger: always_on
description: C++ and MSVC compilation standards for OptiScaler
---

# C++ & MSVC Compiler Guidelines

1. **C++20 `std::format` on Enums**:
   - MSVC's `<format>` implementation does not provide an implicit formatter for enum types when used with formatting specifiers like `{:x}`.
   - **Always** explicitly cast enums to an integer type (e.g. `static_cast<uint32_t>(enumValue)`) before passing them to `std::format`.

2. **File Encodings & Mandatory UTF-8 BOM**:
   - Visual Studio project files (`.vcxproj`, `.vcxproj.filters`) and C++ source/header files in this repo use UTF-8 with BOM (`\xef\xbb\xbf`).
   - Always preserve or restore the BOM when editing these files to prevent unnecessary git diffs and ensure clean MSVC parsing.
   - **Mandatory Pre-Commit Verification**:
     Before committing any modified C++ file, run the following verification snippet to assert that the BOM is present:
     ```bash
     python3 -c "
     with open('<file>', 'rb') as fp:
         assert fp.read(3) == b'\xef\xbb\xbf', '<file> is missing UTF-8 BOM!'
     "
     ```

3. **Clang-Format Standards**:
   - All modified and new C++ source and header files (`.cpp`, `.h`) in `OptiScaler/` and `tests/` must strictly comply with `.clang-format`.
   - Before committing any changes, format modified files with `clang-format -i` and verify clean output with:
     ```bash
     clang-format --dry-run --Werror <modified_files>
     ```
   - Always ensure `clang-format -i` did not strip the UTF-8 BOM; restore the BOM immediately if removed.

## Mandatory Pre-Commit / Pre-Push Gate

Before every `git commit` and `git push`, run:
```bash
clang-format --dry-run --Werror <staged-or-modified C++ files>
```
A commit or push with any clang-format violation is not allowed. Fix formatting first.
