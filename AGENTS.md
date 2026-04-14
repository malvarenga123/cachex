# Agent Guidelines

## Code Audit Findings
* **Monolithic main file**: The `cachex.cpp` file contains 1,200+ lines of code, mixing UI (CLI argument parsing), core testing logic, and SCSI command formatting. It should ideally be broken down.
* **Global State**: The `g_ctx` struct and its accessors effectively implement global state.
* **Magic Numbers**: Many hardcoded SCSI opcodes, page codes, and standard sizes are scattered throughout `cachex.cpp`.
* **Build System**: The project previously used a hardcoded `Makefile`, which has been replaced by `CMakeLists.txt` for easier cross-platform builds.
* **C++ Standard**: The code uses C++17. Modern C++ constructs (like `std::array`, `std::unique_ptr` if applicable, structured bindings) are encouraged.

## Modern Best Practices for this Repository
1. **Separation of Concerns**: Move command-line parsing into its own file/module. Separate the SCSI command definitions from the testing logic.
2. **Build System**: Maintain the `CMakeLists.txt` to support Linux, NetBSD, Windows, etc. Ensure standard compilation flags are applied (`-Wall`, `-Wextra`).
3. **Testing**: Introduce unit tests for the SCSI command generation logic (using a framework like Catch2 or GTest).
4. **Code Formatting**: Use `clang-format` according to the `.clang-format` file in the root directory.

## Current Restructuring Rules
* All source and header files are placed in the `src/` directory.
* Platform headers are named `platform_<os>.h`.
* The main entry point is `src/main.cpp`.

Please adhere to these practices when modifying the code.
