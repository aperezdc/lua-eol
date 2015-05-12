#! /usr/bin/env lua
--
-- libtest-lookup-variable.lua
-- Copyright (C) 2015 Adrian Perez <aperez@igalia.com>
--
-- Distributed under terms of the MIT license.
--

local libtest = require("eris").load("libtest")
assert(libtest, "could not load libtest.so")
assert.Field(libtest, "lookup")

local intvar = libtest:lookup("intvar")
assert.Not.Nil(intvar)
assert.Userdata(intvar)
assert.Not.Callable(intvar)