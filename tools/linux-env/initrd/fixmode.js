// Patches newc cpio header mode fields in place (same length): sets 0100755 for the named entries.
// newc entry = 110-byte ASCII header + name (namesize, NUL-terminated) padded to 4 + data padded to 4.
const fs = require('fs');
const [, , inFile, ...names] = process.argv;
const buf = fs.readFileSync(inFile);
const want = new Set(names);
const pad4 = n => (n + 3) & ~3;
let off = 0;
const patched = [];
while (off + 110 <= buf.length) {
  const magic = buf.toString('ascii', off, off + 6);
  if (magic !== '070701') throw new Error('bad magic at ' + off + ': ' + magic);
  const filesize = parseInt(buf.toString('ascii', off + 54, off + 62), 16);
  const namesize = parseInt(buf.toString('ascii', off + 94, off + 102), 16);
  const name = buf.toString('ascii', off + 110, off + 110 + namesize - 1);
  if (name === 'TRAILER!!!') break;
  if (want.has(name)) {
    buf.write('000081ed', off + 14, 'ascii');
    patched.push(name);
  }
  off += pad4(110 + namesize) + pad4(filesize);
}
fs.writeFileSync(inFile, buf);
console.log('patched:', patched.join(' '));
