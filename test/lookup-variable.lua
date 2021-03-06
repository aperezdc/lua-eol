#! /usr/bin/env lua
--
-- libtest-lookup-variable.lua
-- Copyright (C) 2015 Adrian Perez <aperez@igalia.com>
--
-- Distributed under terms of the MIT license.
--

local libtest = require("eol").load("libtest")
assert(libtest, "could not load libtest.so")

local intvar = libtest.intvar
assert.Not.Nil(intvar)
assert.Userdata(intvar, "org.perezdecastro.eol.Variable")
assert.Not.Callable(intvar)
assert.Equal(libtest, intvar.__library)
