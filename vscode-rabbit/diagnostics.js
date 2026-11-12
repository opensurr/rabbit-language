
function parseCheckOutput(text) {
  const found = [];
  const pattern = /rabbit: (.*?):(\d+): error: (.*)/g;
  let match;
  while ((match = pattern.exec(text)) !== null) {
    found.push({ line: parseInt(match[2], 10), message: match[3].trim() });
  }
  return found;
}

module.exports = { parseCheckOutput };
