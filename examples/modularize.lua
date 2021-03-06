#! /usr/bin/env lua
--
-- modularize.lua
-- Copyright (C) 2015 Adrian Perez <aperez@igalia.com>
--
-- Distributed under terms of the MIT license.
--

local eol           = require("eol")
local eol_load      = eol.load
local eol_type      = eol.type
local _setmetatable = setmetatable
local _rawget       = rawget
local _rawset       = rawset
local _type         = type

--
-- Makes functions and types available (and memoized) in a table,
-- Prefixes are added automatically when looking up items from the
-- library, and different prefixes can be specified for functions
-- and types.
--
-- Usage example:
--
--    png = require "modularize" {
--    	"libpng", prefix = "png_", type_prefix = "png_"
--    }
--
--    color = png.types.color_8()
--    color.red = 255
--    color.green = 127
--
--    print(png.get_header_ver(nil))
--
local function modularize(lib)
	local type_prefix
	local func_prefix
	local library_name

	if _type(lib) == "table" then
		library_name = lib.library or lib[1]
		func_prefix  = lib.function_prefix or lib.prefix or ""
		type_prefix  = lib.type_prefix or lib.prefix or ""
	else
		library_name = lib
		func_prefix  = ""
		type_prefix  = ""
	end

	local library = eol_load(library_name)

	return _setmetatable({ __library = library,
		types = _setmetatable({ __library = library }, {
			__index = function (self, key)
				local t = _rawget(self, key)
				if t == nil then
					t = eol_type(library, type_prefix .. key)
					_rawset(self, key, t or false)
				end
				return t
			end,
		}),
	}, {
		__index = function (self, key)
			local f = _rawget(self, key)
			if f == nil then
				f = library[func_prefix .. key]
				_rawset(self, key, f or false)
			end
			return f
		end,
	});
end

return modularize
