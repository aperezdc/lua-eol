Eris - Fully automatic Lua↔C bridge using DWARF
===============================================

Eris is a fully automatic, run-time, fast binding generator for native
C libraries. It allows using existing libraries directly from
[Lua](http://www.lua.org). Eris uses the [DWARF](http://dwarfstd.org/)
debugging information generated by C compilers.

Eris is licensed under a [MIT-style](http://www.opensource.org/licenses/mit-license.php)
license, see `LICENSE` for the full text.

Compatibility
-------------

Eris is tested and compatible with:

* Lua 5.3


Building
--------

Building Eris requires the following dependencies:

* A reasonable POSIX-ish operating system. Development is done using
  GNU/Linux, other systems might work (YMMV).
* `libdwarf`, version `20140805` (or newer).
* `libelf`, version `0.161` (or newer).
* [Ninja](http://martine.github.com/ninja/) (preferred), or GNU Make.
* *(Optional)* GNU `readline`.

Though GNU Auto*foo* is not used, care has been taken in following its
conventions, so in order to build Eris the following will work:

```sh
./configure
make
```

Or, using Ninja to do the build (preferred):

```sh
./configure
ninja
```

Once building has finished, it is possible to check that everything
is working fine by running the test suite:

```sh
./run-tests
```


Usage
-----

Once built, the `eris` module can be used from Lua:

```lua
-- Find and load the readline library from the standard system directories.
local libreadline = require("eris").load("libreadline")

-- Obtain a handle to the readline() function, which allows calling it.
local readline = libreadline.readline

-- Read a line from standard input using readline() and echo it back.
print(readline("input: "))
```

For more examples, check the the `samples/` subdirectory. Documentation
is available under the `doc/` subdirectory. Run your favourite Markdown
processor on it to read the documentation in HTML.

