# AGENTS.md - ctop Development Guide

## Project Overview

ctop is a lightweight system resource monitor for Linux/macOS, inspired by btop++. It displays CPU, memory, disk I/O, network, and process information in a terminal-based UI using the termbox2 library.

## Build Commands

### Building the Project
```bash
make              # Build the ctop binary (default target)
make all         # Same as make
make clean       # Remove compiled binary
make debug       # Build with debug symbols (-g -DDEBUG)
```

### Installing
```bash
make install     # Install to /usr/local/bin/
make uninstall  # Remove from /usr/local/bin/
```

### Single Test Execution
**Note:** This project has no automated tests. Manual testing is performed by running `./ctop` and verifying the UI displays correctly.

### Build Requirements
- GCC or Clang compiler
- GNU Make
- termbox2.h (included in repository)
- Standard C libraries (libc, libm on Linux)

### Compiler Flags
- `-Wall -Wextra` - Enable strict warnings
- `-O2` - Optimization level 2
- `-std=c99` - C99 standard required
- `-lm` - Math library (Linux only)
- `-D_DARWIN_C_SOURCE` - macOS compatibility

## Code Style Guidelines

### General Principles
- Write clean, readable C code
- Keep functions focused and single-purpose
- Use meaningful variable and function names
- Comment complex logic and non-obvious decisions

### Naming Conventions
- **Functions**: snake_case (e.g., `parse_cpu_stats`, `draw_graph`)
- **Variables**: snake_case (e.g., `g_stats`, `proc->cpu_percent`)
- **Global variables**: Hungarian prefix `g_` (e.g., `g_running`, `g_show_cpu`)
- **Constants/Macros**: SCREAMING_SNAKE_CASE (e.g., `MAX_PROCESSES`, `COLOR_CPU`)
- **Structs/Types**: PascalCase or snake_case_suffix (e.g., `ProcessInfo`, `CoreStat`)
- **Enums**: Prefix with type name (e.g., `SORT_CPU_LAZY`, `SORT_MEM`)

### Brace Style
Use K&R style (opening brace on same line):
```c
void function_example(int arg) {
    if (condition) {
        do_something();
    } else {
        do_other();
    }
}
```

### Indentation
- Use tabs for indentation (consistent with kernel style)
- Align related code visually when it improves readability

### Line Length
- Target 80 characters or less per line
- Can exceed when it improves readability (e.g., function calls with many arguments)

### Comments
- Use C-style comments `/* */` for block comments
- Comment complex algorithms, not trivial operations
- Document function purpose and parameters when non-obvious

### Type Usage
- Use `int` for general integers
- Use `unsigned` types for counts and sizes
- Use `float` for percentages and rates
- Use `char*` for strings, `size_t` for buffer sizes
- Use fixed-width types from `<stdint.h>` when precise size matters (e.g., `int64_t`)

### Error Handling
- Check return values from file operations (`fopen`, `readdir`)
- Use early returns for error conditions when cleanest
- Gracefully handle missing files (e.g., no battery, no network)
- Log errors to stderr when appropriate

### Memory Management
- Use `malloc`/`free` only when dynamic allocation needed
- Prefer fixed-size arrays for known limits (e.g., `MAX_PROCESSES`)
- Always null-terminate strings after `strncpy`
- Use `sizeof()` for buffer sizes rather than hardcoded numbers

### Header Organization
Headers should include:
- `#define` constants and macros
- `typedef` structs and enums
- Function declarations
- Include guards

### Code Organization in ctop.c
1. Includes and defines
2. Global constants (macros)
3. Type definitions (structs, enums)
4. Global variables
5. Static helper functions
6. Parse functions (read /proc files)
7. Draw/render functions
8. Event handling
9. Main function

### Working with termbox2
- Initialize with `tb_init()`, check return value
- Use `tb_print()` for text output
- Use `tb_printf()` for formatted output
- Use `tb_set_cell()` for individual cells
- Call `tb_present()` to render
- Use `tb_peek_event()` for non-blocking input
- Clean up with `tb_shutdown()` on exit

### Common Patterns

#### Parsing /proc Files
```c
FILE *fp = fopen("/proc/somefile", "r");
if (!fp) return;  // Graceful handling

char line[256];
while (fgets(line, sizeof(line), fp)) {
    // Parse line
}
fclose(fp);
```

#### Drawing Sections
```c
void draw_section_header(int x, int y, int num, const char *title, uint32_t color) {
    tb_print(x, y, color, COLOR_BG, "[");
    // ... header content
}

// Use consistent colors from defined constants
uint32_t used_color = g_stats.mem_percent > 80 ? COLOR_HIGH :
                      g_stats.mem_percent > 50 ? COLOR_MED : COLOR_LOW;
```

#### String Handling
```c
// Always use size limits
strncpy(dest, src, dest_size - 1);
dest[dest_size - 1] = '\0';

// Or use snprintf for safety
snprintf(buf, sizeof(buf), "format %s", value);
```

### Things to Avoid
- Do not use `goto` except for cleanup labels
- Avoid global state where possible (this project uses globals for simplicity)
- Do not hardcode magic numbers - use constants
- Avoid deeply nested conditionals
- Do not mix allocation styles (all or nothing)

### Debugging Tips
- Use `make debug` for debug build
- Add debug output with `#ifdef DEBUG`
- Check `/proc` file formats when debugging parsing issues
- Use `tb_width()` and `tb_height()` to check terminal dimensions
