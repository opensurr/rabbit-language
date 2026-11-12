
const KEYWORDS = ['let', 'func', 'if', 'elseif', 'else', 'while', 'repeat', 'until',
  'for', 'in', 'do', 'return', 'break', 'continue', 'and', 'or', 'not', 'true', 'false', 'nil'];

const GLOBALS = ['print', 'input', 'type', 'tostring', 'tonumber', 'error', 'assert',
  'pairs', 'ipairs', 'pcall', 'load', 'require', 'getlib', 'rawget', 'rawset', 'rawequal',
  'wait', 'getglobal', 'setglobal', 'hasglobal', 'getgenv', 'setgenv', 'globals',
  'io', 'http', 'fs', 'net', 'table', 'string', 'math', 'os'];

const LIBRARIES = {
  table: ['insert', 'remove', 'concat', 'sort', 'keys', 'values', 'find', 'contains', 'map', 'filter', 'reduce'],
  string: ['len', 'upper', 'lower', 'sub', 'rep', 'reverse', 'byte', 'char', 'format', 'find', 'match',
    'gmatch', 'gsub', 'split', 'trim', 'replace', 'starts_with', 'ends_with', 'lpad', 'rpad'],
  math: ['floor', 'ceil', 'abs', 'sqrt', 'sin', 'cos', 'tan', 'exp', 'log', 'pow', 'fmod', 'clamp', 'round',
    'max', 'min', 'random', 'randomseed', 'pi', 'huge'],
  os: ['clock', 'time', 'date', 'getenv', 'exit', 'execute', 'capture', 'sleep', 'platform'],
  io: ['write', 'read', 'getchar'],
  fs: ['read', 'write', 'exists', 'remove', 'mkdir', 'list'],
  http: ['get', 'post'],
  net: ['serve'],
};

const OBJECT_MEMBERS = {
  ui: ['clear', 'move', 'color', 'reset', 'text', 'title', 'box', 'progress', 'cursor', 'size',
    'raw_on', 'raw_off', 'key', 'prompt', 'menu', 'gui'],
  rabui: ['clear', 'move', 'color', 'reset', 'text', 'title', 'box', 'progress', 'cursor', 'size',
    'raw_on', 'raw_off', 'key', 'prompt', 'menu', 'gui'],
  gui: ['window', 'label', 'button', 'entry', 'row', 'column', 'get', 'set_text', 'run'],
  web: ['app', 'html', 'text', 'json', 'redirect'],
  json: ['encode', 'decode'],
  html: ['page', 'css', 'el', 'text', 'escape', 'write', 'img', 'br', 'hr', 'div', 'span', 'p',
    'h1', 'h2', 'h3', 'a', 'ul', 'li', 'table', 'tr', 'td', 'th', 'section', 'header', 'footer', 'button', 'form'],
  bridge: ['python', 'node', 'lua'],
  langs: ['python', 'node', 'lua'],
  python: ['eval', 'call', 'run_file', 'has'],
  node: ['eval', 'call', 'run_file', 'has'],
  lua: ['eval', 'run_file', 'has'],
  app: ['get', 'post', 'add', 'static', 'handle', 'listen'],
};

const METHODS = ['upper', 'lower', 'sub', 'rep', 'reverse', 'find', 'match', 'gsub', 'split',
  'trim', 'replace', 'starts_with', 'ends_with', 'len', 'lpad', 'rpad', 'push', 'pop', 'join', 'sort'];

const DOCS = {
  print: 'print(...) - Shows the values separated by tabs, then a new line.',
  input: 'input(prompt) - Shows the prompt and returns the typed line as text, or nil at the end of input.',
  type: 'type(v) - Returns "nil", "boolean", "number", "string", "function", "array" or "table".',
  tostring: 'tostring(v) - Converts a value to text.',
  tonumber: 'tonumber(v) - Converts text to a number, or returns nil.',
  error: 'error(message) - Stops the program with an error (catchable with pcall).',
  assert: 'assert(value, message) - Raises an error when value is false or nil.',
  pairs: 'pairs(t) - Use with for k, v in pairs(t) { }.',
  ipairs: 'ipairs(t) - Same as pairs for arrays.',
  pcall: 'pcall(f, ...) - Runs f and returns {ok = true, value = ...} or {ok = false, error = message}.',
  load: 'load(text) - Compiles text and returns a function.',
  require: 'require("name") - Runs name.rab once and returns what it returns.',
  getlib: 'getlib("name") - Returns a built-in library (table, string, math, os, io, fs, http, net, bridge, web, json, html, rabui) or loads name.rab.',
  rawget: 'rawget(t, k) - Same as t[k].',
  rawset: 'rawset(t, k, v) - Same as t[k] = v.',
  rawequal: 'rawequal(a, b) - Same as a == b.',
  wait: 'wait(seconds) - Pauses the program. Fractions work: wait(0.5).',
  getglobal: 'getglobal(name) - Reads a global variable.',
  setglobal: 'setglobal(name, value) - Creates or changes a global variable.',
  hasglobal: 'hasglobal(name) - true when a global variable exists.',
  getgenv: 'getgenv() - A table copy of every global variable. Use setglobal or setgenv to change globals.',
  setgenv: 'setgenv(table) - Stores each table entry as a global variable.',
  globals: 'globals() - Alphabetical list of global variable names.',
  'table.insert': 'table.insert(list, value) or table.insert(list, position, value). Positions start at 0.',
  'table.remove': 'table.remove(list) or table.remove(list, position). Returns the removed item.',
  'table.concat': 'table.concat(list, separator) - Joins the items as text.',
  'table.sort': 'table.sort(list, compare) - Sorts in place. compare(a, b) returns true when a comes first.',
  'table.keys': 'table.keys(t) - Array of the keys of a table.',
  'table.values': 'table.values(t) - Array of the values of a table.',
  'table.find': 'table.find(list, value) - Position of the value (0-based), or nil.',
  'table.contains': 'table.contains(list, value) - true when the value is in the list.',
  'table.map': 'table.map(list, f) - New list with f(item) for every item.',
  'table.filter': 'table.filter(list, f) - New list with the items where f(item) is true.',
  'table.reduce': 'table.reduce(list, f, start) - Combines items with f(accumulator, item).',
  'string.find': 'string.find(s, pattern, start, plain) - 0-based position of the match, or nil.',
  'string.match': 'string.match(s, pattern) - First capture (or the whole match), or nil.',
  'string.gmatch': 'string.gmatch(s, pattern) - Array of all matches. Use with for m in ... { }.',
  'string.gsub': 'string.gsub(s, pattern, replacement) - Returns the new text.',
  'string.sub': 'string.sub(s, start, stop) - Start included, stop excluded, both from 0.',
  'string.format': 'string.format(fmt, ...) - printf-style formatting, for example %5.2f.',
  'string.split': 'string.split(s, separator) - Array of the parts.',
  'string.trim': 'string.trim(s) - Removes spaces at both ends.',
  'string.lpad': 'string.lpad(s, width, fill) - Pads on the left to the width.',
  'string.rpad': 'string.rpad(s, width, fill) - Pads on the right to the width.',
  'math.floor': 'math.floor(x) - Largest whole number not above x.',
  'math.clamp': 'math.clamp(x, low, high) - Keeps x between low and high.',
  'math.round': 'math.round(x) - Rounds to the nearest whole number.',
  'math.random': 'math.random() gives 0 to 1; math.random(m) gives 1 to m; math.random(m, n) gives m to n.',
  'math.max': 'math.max(...) - Largest of the numbers.',
  'math.min': 'math.min(...) - Smallest of the numbers.',
  'os.capture': 'os.capture(command) - Runs a shell command and returns its output, or nil.',
  'os.execute': 'os.execute(command) - Runs a shell command and returns its exit code.',
  'os.platform': 'os.platform() - "linux", "macos", "bsd" or "other".',
  'fs.read': 'fs.read(path) - File text, or nil.',
  'fs.write': 'fs.write(path, text) - Writes the file. Returns true or false.',
  'fs.mkdir': 'fs.mkdir(path) - Creates a folder. Returns true or false.',
  'fs.list': 'fs.list(folder) - Sorted list of file names in a folder, or nil.',
  'net.serve': 'net.serve(port, handler, host, max_requests) - Starts an HTTP server.',
  'gui.window': 'gui.window(title, width, height) - Window title and size. Call it first.',
  'gui.label': 'gui.label(text) - Adds a line of text. Returns its id.',
  'gui.button': 'gui.button(text, callback) - Adds a button. callback(g) runs when it is clicked. Returns its id.',
  'gui.entry': 'gui.entry(placeholder) - Adds a text box. Returns its id. Read it with gui.get(id).',
  'gui.row': 'gui.row(build) - Puts the widgets made inside build() side by side.',
  'gui.column': 'gui.column(build) - Stacks the widgets made inside build() vertically.',
  'gui.get': 'gui.get(id) - The text in an entry.',
  'gui.set_text': 'gui.set_text(id, text) - Changes a label, button or entry.',
  'gui.run': 'gui.run(port) - Opens the window in your web browser. Stop with Ctrl+C.',
  'ui.gui': 'ui.gui - The window toolkit: gui.window, gui.label, gui.button, gui.entry, gui.run.',
  'ui.box': 'ui.box(row, col, width, height, title) - Draws a box in the terminal.',
  'ui.menu': 'ui.menu(title, items) - Numbered terminal menu. Returns the 0-based choice, or nil.',
  'web.app': 'web.app() - Creates an application with routes: app.get, app.post, app.static, app.listen.',
  'web.html': 'web.html(text) - An HTML response.',
  'web.json': 'web.json(value) - A JSON response.',
  'web.redirect': 'web.redirect(location) - A redirect response.',
  'json.encode': 'json.encode(value) - Turns a Rabbit value into JSON text.',
  'json.decode': 'json.decode(text) - Turns JSON text into a Rabbit value.',
  'html.page': 'html.page({title, css, body}) - A complete HTML page.',
  'html.css': 'html.css(rules) - CSS text from rules like {selector = "h1", style = {color = "red"}}.',
  'html.text': 'html.text(x) - Escapes text so it is shown exactly as typed.',
  'html.write': 'html.write(path, page) - Saves a page to a file.',
  'bridge.python': 'langs.python - Runs Python code. eval, call, run_file and has.',
  python: 'python.eval(code, vars) - Runs Python and returns what it printed. python.call(expr, vars) returns the value to Rabbit.',
  node: 'node.eval(code, vars) - Runs Node.js and returns what it printed. node.call(expr, vars) returns the value to Rabbit.',
  lua: 'lua.eval(code, vars) - Runs Lua and returns what it printed. Lua has no call().',
  'python.eval': 'python.eval(code, vars) - Runs Python code. vars becomes variables in Python. Returns what it printed.',
  'python.call': 'python.call(expr, vars) - Evaluates a Python expression and returns its value to Rabbit.',
  'node.eval': 'node.eval(code, vars) - Runs JavaScript code. vars becomes global variables. Returns what it printed.',
  'node.call': 'node.call(expr, vars) - Evaluates a JavaScript expression and returns its value to Rabbit.',
  'lua.eval': 'lua.eval(code, vars) - Runs Lua code. vars becomes globals. Returns what it printed.',
  push: 'list:push(value) - Adds an item to the end of an array.',
  pop: 'list:pop() - Removes and returns the last item of an array.',
  join: 'list:join(separator) - Joins the items of an array as text.',
  upper: 'text:upper() - Upper-case copy of the text.',
  lower: 'text:lower() - Lower-case copy of the text.',
};

function completionsFor(before) {
  const match = before.match(/([A-Za-z_][A-Za-z0-9_]*)?\s*([.:])\s*[A-Za-z_0-9]*$/);
  if (!match) {
    return { kind: 'general', names: KEYWORDS.concat(GLOBALS, Object.keys(LIBRARIES)) };
  }
  const owner = match[1];
  if (match[2] === ':') {
    return { kind: 'method', names: METHODS.concat(OBJECT_MEMBERS[owner] || []) };
  }
  if (owner && OBJECT_MEMBERS[owner]) {
    return { kind: 'member', names: OBJECT_MEMBERS[owner], prefix: owner + '.' };
  }
  if (owner && LIBRARIES[owner]) {
    return { kind: 'member', names: LIBRARIES[owner], prefix: owner + '.' };
  }
  return { kind: 'none', names: [] };
}

module.exports = { KEYWORDS, GLOBALS, LIBRARIES, OBJECT_MEMBERS, METHODS, DOCS, completionsFor };
