# te

A minimal text editor for embedded systems written in C and using VT100
escape codes for drawing, it also runs on Linux with ncurses.

## Building

### Linux (ncurses)

Requires a C compiler and `ncurses` development headers.

```sh
# Debian/Ubuntu
sudo apt-get install libncurses-dev

gcc -o te te.c -lncurses -DMAIN -DCURSES
```

### Embedded targets

For non-Unix embedded systems that don't have `fopen`/`fread`/`fwrite`
(or `ncurses`), build with `-DEMBEDDED` instead of `-DCURSES`.
You will need to provide implementations of:

- `int fs_size(char *filename)` — return a file's size in bytes
- `char *fs_mallocfile(char *filename)` — read a file into a newly
  allocated buffer
- `int fs_write_file(char *filename, char *buf, int len)` — write
  `len` bytes from `buf` to a file, returning the number of bytes
  written
- `int getch(void)` — read one character. If your main loop calls
  `te_yield()` on its own dedicated thread/task with nothing else to
  do, `getch()` can block until a key is available. If `te_yield()` is
  instead called cooperatively from a shared main loop (e.g. once per
  iteration alongside other tasks), `getch()` must be non-blocking:
  return `EOF` (or `0`) immediately when no key is waiting, and
  `te_yield()` will return right away so the rest of the loop can run.

## Usage

```sh
./te <filename>
```

`te` opens (or creates) `<filename>` and drops you straight into editing.

On an embedded system you would instead call `te_edit(filename)`.

### Editing

- Type to insert text at the cursor.
- **Enter** splits the line at the cursor.
- **Backspace** deletes the character before the cursor, or merges the
  current line into the previous one if the cursor is at the start of a line.
- Arrow keys move the cursor:
  - **Up/Down** move by line, remembering your column so moving through a
    shorter line and back doesn't lose your place (like vim's insert mode).
  - **Left/Right** wrap to the previous/next line at line boundaries.
  - **Page Up/Page Down** jump a full screen at a time, also remembering
    your column.
  - **Esc ^** jumps to the start of the current line; **Esc $** jumps to
    the end (vi-style). **Home**/**End** do the same.
- Lines and files longer than the terminal will scroll automatically,
  both vertically and horizontally, to keep the cursor in view.

### Commands

Commands are entered with `Esc` followed by `:`, then either a single
letter or a line number:

| Keys       | Action                                  |
|------------|------------------------------------------|
| `Esc :w`   | Save the file                            |
| `Esc :q`   | Quit                                     |
| `Esc :<N><Enter>` | Jump to line `N` (1-based, vi-style — e.g. `Esc :1` jumps to the top) |

## Status line

The bottom row of the terminal shows the current state after every
keystroke, in the form:

```
te <filename> l<line count> s<state> x<column> y<line>
```

`s<state>` is the internal input state, useful for telling whether a
keystroke will be inserted as text or interpreted as part of a command.

If `s` isn't `0`, keys you type won't be inserted as text — they're being
read as part of an escape sequence or command instead.

## License

The contents of this repo are released under the [Lone Dynamics Open License](LICENSE.md).
