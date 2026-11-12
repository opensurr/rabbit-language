
const KEYWORDS = new Set(['let', 'func', 'if', 'elseif', 'else', 'while', 'repeat', 'until', 'for', 'in',
  'do', 'return', 'break', 'continue', 'and', 'or', 'not', 'true', 'false', 'nil']);

const BUILTIN_NAMES = new Set(['print', 'input', 'type', 'tostring', 'tonumber', 'error', 'assert', 'pairs',
  'ipairs', 'pcall', 'load', 'require', 'getlib', 'rawget', 'rawset', 'rawequal', 'wait', 'getglobal',
  'setglobal', 'hasglobal', 'getgenv', 'setgenv', 'globals', 'io', 'http', 'fs', 'net', 'table', 'string',
  'math', 'os', 'self']);

const CALL_TYPES = { tostring: 'string', tonumber: 'number', type: 'string', input: 'string',
  getlib: 'library', require: 'unknown', pcall: 'table', globals: 'array', getgenv: 'table',
  getglobal: 'unknown', ['string.format']: 'string', ['string.upper']: 'string', ['string.lower']: 'string',
  ['string.sub']: 'string', ['string.rep']: 'string', ['string.trim']: 'string', ['string.replace']: 'string',
  ['string.split']: 'array', ['string.gmatch']: 'array', ['table.keys']: 'array', ['table.values']: 'array',
  ['table.map']: 'array', ['table.filter']: 'array', ['math.floor']: 'number', ['math.random']: 'number',
  ['math.sqrt']: 'number', ['math.max']: 'number', ['math.min']: 'number', ['os.time']: 'number',
  ['os.clock']: 'number', ['os.date']: 'string', ['fs.read']: 'string', ['fs.exists']: 'boolean',
  ['fs.write']: 'boolean', ['json.encode']: 'string', ['json.decode']: 'unknown' };

function blankStringsAndComments(text) {
  let out = '';
  let i = 0;
  while (i < text.length) {
    const c = text[i];
    if (c === '-' && text[i + 1] === '-') {
      if (text[i + 2] === '[' && text[i + 3] === '[') {
        const end = text.indexOf(']]', i + 4);
        const stop = end < 0 ? text.length : end + 2;
        out += text.slice(i, stop).replace(/[^\n]/g, ' ');
        i = stop;
      } else {
        const end = text.indexOf('\n', i);
        const stop = end < 0 ? text.length : end;
        out += ' '.repeat(stop - i);
        i = stop;
      }
    } else if (c === '"') {
      let j = i + 1;
      while (j < text.length && text[j] !== '"' && text[j] !== '\n') {
        j += text[j] === '\\' ? 2 : 1;
      }
      const stop = Math.min(j + 1, text.length);
      out += '"' + ' '.repeat(Math.max(0, stop - i - 2)) + (stop - i >= 2 ? '"' : '');
      i = stop;
    } else {
      out += c;
      i++;
    }
  }
  return out;
}

function expressionAt(text, start) {
  let depth = 0;
  let i = start;
  while (i < text.length) {
    const c = text[i];
    if (c === '"') {
      i++;
      while (i < text.length && text[i] !== '"' && text[i] !== '\n') i += text[i] === '\\' ? 2 : 1;
      i++;
      continue;
    }
    if (c === '-' && text[i + 1] === '-') break;
    if ('([{'.includes(c)) depth++;
    if (')]}'.includes(c)) depth--;
    if (c === '\n' && depth <= 0) break;
    i++;
  }
  return text.slice(start, i).trim();
}

function typeOf(expr) {
  const e = expr.trim();
  if (e === '' ) return 'unknown';
  if (/^nil\b/.test(e)) return 'nil';
  if (/^(true|false)\b/.test(e) || /^not\b/.test(e)) return 'boolean';
  if (/^-?\d/.test(e)) return 'number';
  if (/^"/.test(e)) return 'string';
  if (/^\{/.test(e)) return 'table';
  if (/^\[/.test(e)) return 'array';
  if (/^func\b/.test(e)) return 'function';
  if (/^#/.test(e)) return 'number';
  const call = e.match(/^([A-Za-z_][\w.]*)\s*\(/);
  if (call && Object.prototype.hasOwnProperty.call(CALL_TYPES, call[1])) return CALL_TYPES[call[1]];
  if (/^[A-Za-z_][\w.]*\s*\.\./.test(e) || /"\s*\.\./.test(e)) return 'string';
  if (/^[\w.()\s+\-*/%#]+$/.test(e) && /[+\-*/%]/.test(e)) return 'number';
  return 'unknown';
}

function tableFields(literal) {
  const fields = [];
  const pattern = /(?:^|[{,\n])\s*([A-Za-z_]\w*)\s*[:=](?!=)/g;
  let m;
  while ((m = pattern.exec(literal)) !== null) {
    if (!fields.includes(m[1])) fields.push(m[1]);
  }
  return fields;
}

function lineOf(text, index) {
  let line = 1;
  for (let i = 0; i < index && i < text.length; i++) if (text[i] === '\n') line++;
  return line;
}

function analyzeVariables(text) {
  const clean = blankStringsAndComments(text);
  const vars = new Map();
  const note = (name, index, type, fields) => {
    if (!vars.has(name)) {
      vars.set(name, { type, line: lineOf(text, index), fields: [], history: [] });
    }
    const v = vars.get(name);
    v.history.push({ index, type, line: lineOf(text, index) });
    v.type = type;
    for (const f of fields || []) if (!v.fields.includes(f)) v.fields.push(f);
  };

  const declare = /(^|\n)[ \t]*let[ \t]+([A-Za-z_]\w*)[ \t]*(=)?/g;
  let m;
  while ((m = declare.exec(clean)) !== null) {
    const name = m[2];
    const at = m.index + m[0].length;
    if (!m[3]) { note(name, m.index, 'nil', []); continue; }
    const expr = expressionAt(text, at);
    const type = typeOf(expr);
    note(name, m.index, type, type === 'table' ? tableFields(expr) : []);
  }

  const assign = /(^|\n)[ \t]*([A-Za-z_]\w*)[ \t]*=(?!=)/g;
  while ((m = assign.exec(clean)) !== null) {
    const name = m[2];
    if (KEYWORDS.has(name)) continue;
    const at = m.index + m[0].length;
    const expr = expressionAt(text, at);
    if (vars.has(name)) note(name, m.index, typeOf(expr), type_is_table(expr) ? tableFields(expr) : []);
  }

  for (const [name, v] of vars) {
    const field = new RegExp('\\b' + name + '\\.([A-Za-z_]\\w*)[ \\t]*=(?!=)', 'g');
    let f;
    while ((f = field.exec(clean)) !== null) {
      if (!v.fields.includes(f[1])) v.fields.push(f[1]);
    }
  }
  return { vars };
}

function type_is_table(expr) { return /^\{/.test(expr.trim()); }

function typeAt(analysis, name, index) {
  const v = analysis.vars.get(name);
  if (!v) return null;
  let current = null;
  for (const h of v.history) {
    if (h.index <= index) current = h;
  }
  return current ? { type: current.type, line: current.line, fields: v.fields } : null;
}

function undefinedNames(text, extraKnown) {
  const clean = blankStringsAndComments(text);
  const known = new Set([...BUILTIN_NAMES, ...(extraKnown || [])]);

  const declared = /(?:let|func|for)[ \t]+([A-Za-z_]\w*)/g;
  const secondLoopName = /for[ \t]+[A-Za-z_]\w*[ \t]*,[ \t]*([A-Za-z_]\w*)/g;
  const globalName = /setglobal[ \t]*\([ \t]*"([A-Za-z_]\w*)"/g;
  let m;
  while ((m = declared.exec(clean)) !== null) known.add(m[1]);
  while ((m = secondLoopName.exec(clean)) !== null) known.add(m[1]);
  while ((m = globalName.exec(text)) !== null) known.add(m[1]);
  const params = /func[ \t]*(?:[A-Za-z_]\w*)?[ \t]*\(([^)]*)\)/g;
  while ((m = params.exec(clean)) !== null) {
    for (const p of m[1].split(',')) { const n = p.trim(); if (n) known.add(n); }
  }
  const assigned = /(^|\n)[ \t]*([A-Za-z_]\w*)[ \t]*=(?!=)/g;
  while ((m = assigned.exec(clean)) !== null) known.add(m[2]);
  const keyNames = /([A-Za-z_]\w*)[ \t]*:(?!:)/g;
  while ((m = keyNames.exec(clean)) !== null) known.add(m[1]);
  const keyEq = /([A-Za-z_]\w*)[ \t]*=(?!=)/g;
  while ((m = keyEq.exec(clean)) !== null) known.add(m[1]);

  const problems = [];
  const use = /(^|[^\w.:])([A-Za-z_]\w*)/g;
  const lines = clean.split('\n');
  lines.forEach((line, i) => {
    let u;
    use.lastIndex = 0;
    while ((u = use.exec(line)) !== null) {
      const name = u[2];
      if (KEYWORDS.has(name) || known.has(name)) continue;
      const after = line.slice(u.index + u[0].length);
      if (/^\s*:(?!:)/.test(after)) continue;
      problems.push({ name, line: i + 1, column: u.index + u[1].length });
    }
  });
  return problems;
}

function wordAt(lineText, column) {
  let s = column, e = column;
  while (s > 0 && /\w/.test(lineText[s - 1])) s--;
  while (e < lineText.length && /\w/.test(lineText[e])) e++;
  return { word: lineText.slice(s, e), start: s, end: e };
}

module.exports = { analyzeVariables, typeAt, undefinedNames, typeOf, tableFields, blankStringsAndComments, wordAt, lineOf };
