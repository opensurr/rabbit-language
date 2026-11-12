
const vscode = require('vscode');
const { execFile, spawn } = require('child_process');
const { parseCheckOutput } = require('./diagnostics');
const { DOCS, KEYWORDS, GLOBALS, completionsFor } = require('./docs');
const { analyzeVariables, typeAt, undefinedNames, wordAt } = require('./analyze');

let warnedMissing = false;

function rabbitPath() {
  return vscode.workspace.getConfiguration('rabbit').get('path', 'rabbit');
}

function warnMissingOnce() {
  if (!warnedMissing) {
    warnedMissing = true;
    vscode.window.showWarningMessage(
      'Rabbit program not found. Install Rabbit or set "rabbit.path" in the settings.');
  }
}

function checkText(text, callback) {
  const child = spawn(rabbitPath(), ['--check', '-']);
  let stderr = '';
  child.stderr.on('data', (chunk) => { stderr += chunk; });
  child.on('error', (err) => {
    if (err.code === 'ENOENT') warnMissingOnce();
    callback(null);
  });
  child.on('close', () => callback(parseCheckOutput(stderr)));
  child.stdin.end(text);
}

function buildDiagnostics(doc, syntaxErrors) {
  const items = [];
  for (const found of syntaxErrors) {
    const line = Math.max(0, Math.min(found.line - 1, doc.lineCount - 1));
    const lineText = doc.lineAt(line).text;
    const start = Math.max(0, lineText.search(/\S/));
    const range = new vscode.Range(line, start, line, Math.max(start + 1, lineText.length));
    items.push(new vscode.Diagnostic(range, found.message, vscode.DiagnosticSeverity.Error));
  }
  for (const w of undefinedNames(doc.getText())) {
    const line = Math.max(0, Math.min(w.line - 1, doc.lineCount - 1));
    const range = new vscode.Range(line, w.column, line, w.column + w.name.length);
    const message = "'" + w.name + "' is not defined in this file. Is it a typo?";
    items.push(new vscode.Diagnostic(range, message, vscode.DiagnosticSeverity.Warning));
  }
  return items;
}

function activate(context) {
  const problems = vscode.languages.createDiagnosticCollection('rabbit');
  context.subscriptions.push(problems);
  const timers = new Map();

  function refresh(doc, delay) {
    if (doc.languageId !== 'rabbit' || doc.uri.scheme !== 'file') return;
    const key = doc.uri.toString();
    clearTimeout(timers.get(key));
    timers.set(key, setTimeout(() => {
      checkText(doc.getText(), (syntaxErrors) => {
        if (syntaxErrors === null) return;
        problems.set(doc.uri, buildDiagnostics(doc, syntaxErrors));
      });
    }, delay));
  }

  context.subscriptions.push(
    vscode.workspace.onDidOpenTextDocument((doc) => refresh(doc, 0)),
    vscode.workspace.onDidChangeTextDocument((event) => refresh(event.document, 350)),
    vscode.workspace.onDidSaveTextDocument((doc) => refresh(doc, 0)),
    vscode.workspace.onDidCloseTextDocument((doc) => {
      problems.delete(doc.uri);
      timers.delete(doc.uri.toString());
    }),
  );
  vscode.workspace.textDocuments.forEach((doc) => refresh(doc, 0));

  context.subscriptions.push(vscode.languages.registerCompletionItemProvider('rabbit', {
    provideCompletionItems(doc, pos) {
      const before = doc.lineAt(pos).text.substring(0, pos.character);
      const offset = doc.offsetAt(pos);
      const analysis = analyzeVariables(doc.getText());

      const dot = before.match(/([A-Za-z_]\w*)\s*\.\s*[A-Za-z_]*$/);
      if (dot) {
        const known = typeAt(analysis, dot[1], offset);
        if (known && known.type === 'table') {
          return known.fields.map((field) => {
            const item = new vscode.CompletionItem(field, vscode.CompletionItemKind.Field);
            item.detail = 'field of ' + dot[1];
            return item;
          });
        }
      }

      const found = completionsFor(before);
      const items = found.names.map((name) => {
        const kinds = { general: vscode.CompletionItemKind.Keyword,
          member: vscode.CompletionItemKind.Function, method: vscode.CompletionItemKind.Method };
        let kind = kinds[found.kind] || vscode.CompletionItemKind.Function;
        if (found.kind === 'general' && !KEYWORDS.includes(name) && !GLOBALS.includes(name)) {
          kind = vscode.CompletionItemKind.Module;
        }
        const item = new vscode.CompletionItem(name, kind);
        const text = DOCS[(found.prefix || '') + name] || DOCS[name];
        if (text) item.documentation = new vscode.MarkdownString(text);
        return item;
      });

      if (found.kind === 'general') {
        for (const [name] of analysis.vars) {
          const known = typeAt(analysis, name, offset);
          if (!known) continue;
          const item = new vscode.CompletionItem(name, vscode.CompletionItemKind.Variable);
          item.detail = known.type + ' (line ' + known.line + ')';
          items.push(item);
        }
      }
      return items;
    },
  }, '.', ':'));

  context.subscriptions.push(vscode.languages.registerHoverProvider('rabbit', {
    provideHover(doc, pos) {
      const lineText = doc.lineAt(pos.line).text;
      const range = doc.getWordRangeAtPosition(pos, /[A-Za-z_][A-Za-z0-9_]*(\.[A-Za-z_][A-Za-z0-9_]*)?/);
      if (!range) return undefined;
      const dotted = doc.getText(range);
      if (DOCS[dotted]) return new vscode.Hover(new vscode.MarkdownString(DOCS[dotted]));

      const word = wordAt(lineText, pos.character).word;
      const analysis = analyzeVariables(doc.getText());
      const offset = doc.offsetAt(pos);

      const owner = lineText.substring(0, wordAt(lineText, pos.character).start).match(/([A-Za-z_]\w*)\s*\.\s*$/);
      if (owner) {
        const known = typeAt(analysis, owner[1], offset);
        if (known && known.type === 'table' && known.fields.includes(word)) {
          return new vscode.Hover(new vscode.MarkdownString(
            '**' + word + '** is a field of **' + owner[1] + '** (table)'));
        }
      }

      const known = typeAt(analysis, word, offset);
      if (known) {
        let text = '**' + word + '** is a **' + known.type + '**';
        if (known.type === 'table' && known.fields.length) {
          text += '\n\nFields: ' + known.fields.map((f) => '`' + f + '`').join(', ');
        }
        text += '\n\nSet on line ' + known.line + '.';
        return new vscode.Hover(new vscode.MarkdownString(text));
      }
      return DOCS[word] ? new vscode.Hover(new vscode.MarkdownString(DOCS[word])) : undefined;
    },
  }));

  context.subscriptions.push(vscode.commands.registerCommand('rabbit.run', () => {
    const editor = vscode.window.activeTextEditor;
    if (!editor || editor.document.languageId !== 'rabbit') {
      vscode.window.showInformationMessage('Open a .rab file first.');
      return;
    }
    editor.document.save().then(() => {
      let term = vscode.window.terminals.find((t) => t.name === 'Rabbit');
      if (!term) term = vscode.window.createTerminal('Rabbit');
      term.show();
      term.sendText(rabbitPath() + ' ' + JSON.stringify(editor.document.fileName));
    });
  }));

  context.subscriptions.push(vscode.commands.registerCommand('rabbit.check', () => {
    const editor = vscode.window.activeTextEditor;
    if (editor) refresh(editor.document, 0);
  }));
}

function deactivate() {}

module.exports = { activate, deactivate };
