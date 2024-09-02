# SQLsalt

SQLsalt is a [VFS](https://www.sqlite.org/vfs.html) [extension](https://www.sqlite.org/loadext.html) 
for SQLite that uses [libsodium](https://github.com/jedisct1/libsodium) to encrypt files at rest 
using modern cryptographic standards.

SQLsalt is tested against SQLite's TCL testsuite. Due to the nature of the extension, some tests
that e.g. depend on the binary format on disk cannot succeed, therefore deviations from the 
testsuite are explicitly documented.