# Lua 5.4.9 (vendored)

From https://www.lua.org/ftp/lua-5.4.9.tar.gz
(SHA-256 2335b6c582a52654f94612bf10d2f4672805d05329aa6568b1d8cd9e5c6fb8e6), MIT licence (LICENSE.txt).

Only the core and the base, coroutine, math, string, table and utf8 libraries.
The `.c` files are renamed `.c.h` (so Arduino copies them but does not compile them alone) and compiled together, as C++, by
`../../lua_all.cpp` (the layout of Lua's own onelua.c): as C++ Lua raises
errors as exceptions, so the C++ objects of the bindings are destroyed when a
script fails.  Not taken: io, os, package (loadlib), debug, lua.c, luac.c,
linit.c.  Sources unchanged except luaconf.h, which defines LLONG_MAX from GCC's __LONG_LONG_MAX__ when the toolchain leaves it out.
