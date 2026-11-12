```markdown
# Rabbit

Rabbit is a small, readable programming language with an interpreter written in C.
It borrows ideas from Lua — short syntax, `..` for string concatenation, `--`
comments, and Lua-style patterns — and from JavaScript — braces, `func`, `??`,
and `+=`.

```rabbit
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

Rabbit works on Linux, macOS, and BSD. On Windows, use WSL.

```sh
make
sudo make install
```

This installs `rabbit`, `rabrun`, and `rabget`, along with the libraries in `lib/`.

```sh
rabbit program.rab              # run a program
rabbit --check program.rab      # report syntax errors only
make test                       # run the test suite
```

## Libraries

Load a library with `getlib("name")`:

- **`web`** — routes, JSON responses, and static files (uses `net.serve`)
- **`json`** — encode and decode JSON
- **`html`** — build HTML pages and CSS
- **`bridge`** — run Python, Node.js, and Lua code with shared values
- **`rabui`** — terminal interface helpers, plus `rabui.gui` for a browser window

## Packages

`rabget` installs Python packages into a private environment at `~/.rabbit/venv`,
so your system Python is left untouched.

```sh
rabget requests
rabget node axios
rabget lua dkjson
```

## Editor support

`vscode-rabbit/` is a VS Code extension with syntax highlighting, live error
underlines, type-aware hover and completions, snippets, and an icon theme.

Build it with:

```sh
make vsix
```

Then install `rabbit.vsix` in VS Code.

## Security

**Rabbit is not a sandbox.** Scripts run with the same permissions as the user
who launches them. They can read and write files, execute system commands, and
make network requests.

**Do not run Rabbit scripts you do not trust.** Unauthorized users may abuse
Rabbit scripts to gain full access to your system, so review code carefully and
scan anything suspicious. Reporting issues or vulnerabilities helps keep you and
others safe.

Read [SECURITY.md](SECURITY.md) before running programs you did not write.

## License

MIT. See [LICENSE](LICENSE).
```

