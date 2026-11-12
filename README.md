# Rabbit

Rabbit is a small, readable programming language with an interpreter written in C.
It takes ideas from Lua (short syntax, `..` for text, `--` comments, Lua-style
patterns) and from JavaScript (braces, `func`, `??`, `+=`).

```
let user = {name = "Bunny", age = 16}
func greet(who) {
  return "Hello, " .. who .. "!"
}
print(greet(user.name))
for key, value in user {
  print(key, value)
}
```

## Build and install

Linux, macOS, and BSD (on Windows, use WSL).

    make
    sudo make install

This installs `rabbit`, `rabrun`, and `rabget`, plus the libraries in `lib/`.

    rabbit program.rab        run a program
    rabbit --check program.rab   report syntax errors only
    make test                 run the test suite

## Libraries

Loaded with `getlib("name")`:

- `web`: routes, JSON responses, static files (uses `net.serve`)
- `json`: encode and decode JSON
- `html`: build HTML pages and CSS
- `bridge`: run Python, Node.js, and Lua code, with shared values
- `rabui`: terminal interface helpers, and `rabui.gui` for a browser window

## Packages

`rabget` installs Python packages into a private environment in `~/.rabbit/venv`,
so the system Python is not changed.

    rabget requests
    rabget node axios
    rabget lua dkjson

## Editor support

`vscode-rabbit/` is a VS Code extension with syntax highlighting, live error
underlines, type-aware hover and completions, snippets, and an icon theme.
Build it with `make vsix` and install `rabbit.vsix` in VS Code.

## Security

Read [SECURITY.md](SECURITY.md) before running programs you did not write.

## License

MIT. See [LICENSE](LICENSE).
