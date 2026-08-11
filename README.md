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

Three headers must exist on your include path for this build to
compile: `fs.h` (declares the four functions below).

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

**Opening a file that doesn't exist yet is how you create a new one** —
`te_edit()` doesn't treat this as an error. It calls `te_init()`
first, which always sets up an empty document regardless of whether a
file exists, then `te_load()`, which correctly does nothing (rather
than failing) if there's nothing to load. `fs_size()`/`fs_mallocfile()`
returning "not found" for a missing file is the normal, expected case
here — you don't need to check existence yourself before calling
`te_edit()` just to allow creating new files.

**`te_edit()` has no size limit of its own.** `fs_mallocfile()` is
expected to load the *whole* file into one buffer, however large that
is — there's no streaming or paging. If your target needs a ceiling
(most memory-constrained embedded targets do), check `fs_size()`
yourself and decide whether to call `te_edit()` at all *before* you
call it; te.c has no way to refuse on your behalf.

A load failure for a file that *does* exist (e.g. `fs_mallocfile()`
genuinely returning `NULL` for a real allocation failure, as opposed
to a missing file) prints an error and returns control to your caller
under `-DEMBEDDED`, rather than calling `exit()` the way the desktop/
`-DCURSES` build does — there's typically no OS underneath bare-metal
firmware for `exit()` to return to, so calling it would take your
whole device down rather than just this editor session. Note that
`te_edit()` returns `void`: this failure path and a normal, successful
quit are not distinguishable from the return alone. If you need to
tell them apart, check the file's existence/size yourself before and
after the call.

The embedded build also has no way to query terminal size at runtime;
it's fixed at compile time via `TE_DEFAULT_ROWS`/`TE_DEFAULT_COLS`
(24×80 by default) — override these if your target's screen is a
different size.

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
