# Security

## What Rabbit is and is not protected from

A Rabbit program runs with the same permissions as the user who started it.
Rabbit is not a sandbox. Scripts can read and write files, start programs
(`os.execute`, `os.capture`), and make network requests (`http`). Only run
programs you trust, the same as any script.

## Built-in protections


- Numbers from programs are converted to integers safely. NaN and very large
  values cannot cause undefined behaviour.
- Nested function calls are limited (2000 levels), and deeply nested code is
  rejected (300 levels). The interpreter reports an error instead of crashing.
- `string.rep` refuses outputs above 256 MB.
- `http.get` and `http.post` accept only `http://` and `https://` addresses,
  so an address cannot be read as a command-line option.
- Shell arguments built from program text are quoted.
- Temporary files are created with `mktemp`, so a predictable name cannot be
  reused by another user.
- Web responses cannot inject extra headers: line breaks are removed from header
  names and values.
- Circular `require` is detected and reported.

## Web server

`net.serve` and the `web` library have no authentication and no TLS. The default
address is `127.0.0.1`, so only this computer can connect. If you listen on
`0.0.0.0`, anyone on your network can reach the server. Put it behind a reverse
proxy with authentication, or use it only on a trusted network. The server
handles one request at a time, so a slow client can delay others.

## Reporting a problem

Please report security problems privately to the maintainer before publishing
details, so that a fix can be released first.

