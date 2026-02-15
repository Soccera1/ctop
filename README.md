# ctop

A lightweight system resource monitor for Linux and macOS, written in C and inspired by btop++.

## Features

- **CPU monitoring** - Per-core and overall usage with history graphs
- **Memory monitoring** - Used, available, and total memory
- **Disk I/O** - Read/write speeds and usage per disk
- **Network monitoring** - Upload/download speeds with history
- **Process list** - Sortable process view with CPU and memory usage
- **Battery status** - Current charge level (when available)

## Requirements

- GCC or Clang compiler
- GNU Make
- Linux or macOS
- termbox2 (vendored in repository)

## Building

```bash
make              # Build the binary
make debug        # Build with debug symbols
make clean        # Remove compiled binary
```

## Installing

```bash
make install         # Install to /usr/local/bin/
make PREFIX=/opt install  # Install to /opt/bin/
make uninstall       # Remove from installation directory
```

## Usage

```bash
./ctop           # Run the monitor
```

### Keyboard Shortcuts

| Key | Action |
|-----|--------|
| `1` - `5` | Toggle CPU, Memory, Disks, Network, Processes panes |
| `Ctrl+F` | Cycle sort mode forward |
| `Ctrl+B` | Cycle sort mode backward |
| `Arrow Up/Down` or `Ctrl+P/Ctrl+N` | Navigate process list |
| `Page Up/Down` or `Ctrl+V/Alt+v` | Jump 10 processes up/down |
| `Home` or `Ctrl+A` | Jump to first process |
| `End` or `Ctrl+E` | Jump to last process |
| `q` / `Q` / `Esc` / `Ctrl+C` | Quit |

## License

GPL-3.0 - See [LICENSE](LICENSE) file.

ctop includes [termbox2](https://github.com/termbox/termbox2) (MIT licensed) as a vendored dependency.
