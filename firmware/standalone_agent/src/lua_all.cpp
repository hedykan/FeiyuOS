// Lua 5.4.9 (third_party/lua) compiled as C++ in one unit, the layout of
// Lua's own onelua.c.  As C++, Lua raises errors as exceptions instead of
// longjmp, so the C++ objects (String, cJSON wrappers) of lua_vm.cpp's
// bindings are destroyed when a script fails.  Internal functions are static;
// only the lua_* / luaL_* / luaopen_* API is visible.
#include "third_party/lua/lprefix.h"

#include <assert.h>
#include <ctype.h>
#include <errno.h>
#include <float.h>
#include <limits.h>
#include <locale.h>
#include <math.h>
#include <setjmp.h>
#include <signal.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define LUA_CORE
#define LUA_LIB
#define ltable_c
#define lvm_c
#include "third_party/lua/luaconf.h"

#undef LUAI_FUNC
#undef LUAI_DDEC
#undef LUAI_DDEF
#define LUAI_FUNC static
#define LUAI_DDEC(def) /* empty */
#define LUAI_DDEF static

// core
#include "third_party/lua/lzio.c.h"
#include "third_party/lua/lctype.c.h"
#include "third_party/lua/lopcodes.c.h"
#include "third_party/lua/lmem.c.h"
#include "third_party/lua/lundump.c.h"
#include "third_party/lua/ldump.c.h"
#include "third_party/lua/lstate.c.h"
#include "third_party/lua/lgc.c.h"
#include "third_party/lua/llex.c.h"
#include "third_party/lua/lcode.c.h"
#include "third_party/lua/lparser.c.h"
#include "third_party/lua/ldebug.c.h"
#include "third_party/lua/lfunc.c.h"
#include "third_party/lua/lobject.c.h"
#include "third_party/lua/ltm.c.h"
#include "third_party/lua/lstring.c.h"
#include "third_party/lua/ltable.c.h"
#include "third_party/lua/ldo.c.h"
#include "third_party/lua/lvm.c.h"
#include "third_party/lua/lapi.c.h"

// the libraries scripts get (no io, os, package, debug)
#include "third_party/lua/lauxlib.c.h"
#include "third_party/lua/lbaselib.c.h"
#include "third_party/lua/lcorolib.c.h"
#include "third_party/lua/lmathlib.c.h"
#include "third_party/lua/lstrlib.c.h"
#include "third_party/lua/ltablib.c.h"
#include "third_party/lua/lutf8lib.c.h"
