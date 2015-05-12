/*
 * eris-module.c
 * Copyright (C) 2015 Adrian Perez <aperez@igalia.com>
 *
 * Distributed under terms of the MIT license.
 */

#include "lua/src/lua.h"
#include "lua/src/lauxlib.h"

#include "eris-trace.h"
#include "eris-util.h"

#include <libdwarf/libdwarf.h>
#include <libdwarf/dwarf.h>
#include <libelf.h>
#include <dlfcn.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <stdbool.h>
#include <limits.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <errno.h>


#ifndef ERIS_LIB_SUFFIX
#define ERIS_LIB_SUFFIX ".so"
#endif /* !ERIS_LIB_SUFFIX */


/*
 * Data needed for each library loaded by "eris.load()".
 */
typedef struct {
    int           fd;
    void         *dl;
    intptr_t      dl_diff;

    Dwarf_Debug   d_debug;
    Dwarf_Global *d_globals;
    Dwarf_Signed  d_num_globals;
} ErisLibrary;


static const char ERIS_LIBRARY[]  = "org.perezdecastro.eris.Library";


static inline ErisLibrary*
to_eris_library (lua_State *L)
{
    return (ErisLibrary*) luaL_checkudata (L, 1, ERIS_LIBRARY);
}


static bool find_library_base_address (ErisLibrary *el);
static Dwarf_Die lookup_die (ErisLibrary *el, const void *address, const char *name);


/*
 * FIXME: This makes ErisVariable/ErisFunction keep a reference to their
 *        corresponding ErisLibrary, which itself might be GCd while there
 *        are still live references to it!
 */
#define ERIS_COMMON_FIELDS \
    ErisLibrary *library;  \
    void        *address;  \
    char        *name


/*
 * Any structure that uses ERIS_COMMON_FIELDS at its start can be casted to
 * this struct type.
 */
typedef struct {
    ERIS_COMMON_FIELDS;
} ErisSymbol;


typedef struct {
    ERIS_COMMON_FIELDS;
    Dwarf_Die d_die;
} ErisFunction;

typedef struct {
    ERIS_COMMON_FIELDS;
} ErisVariable;


static const char ERIS_FUNCTION[] = "org.perezdecastro.eris.Function";
static const char ERIS_VARIABLE[] = "org.perezdecastro.eris.Variable";


static inline ErisFunction*
to_eris_function (lua_State *L)
{
    return (ErisFunction*) luaL_checkudata (L, 1, ERIS_FUNCTION);
}

static inline ErisVariable*
to_eris_variable (lua_State *L)
{
    return (ErisVariable*) luaL_checkudata (L, 1, ERIS_VARIABLE);
}


static bool
find_library (const char *name, char path[PATH_MAX])
{
    /* TODO: Handle multilib systems (lib32 vs. lib64) */
    static const char *search_paths[] = {
        "", /* For relative and absolute paths. */
        "/lib/",
        "/usr/lib/",
        "/usr/local/lib/",
    };

    char try_path[PATH_MAX];
    for (size_t i = 0; i < LENGTH_OF (search_paths); i++) {
        if (snprintf (try_path, PATH_MAX, "%s%s" ERIS_LIB_SUFFIX,
                      search_paths[i], name) > PATH_MAX) {
            return false;
        }

        struct stat sb;
        if (realpath (try_path, path) &&
            stat (path, &sb) == 0 && S_ISREG (sb.st_mode) &&
            ((sb.st_mode & (S_IRUSR | S_IRGRP | S_IROTH)) != 0)) {
            return true;
        }
    }

    return false;
}



static int
eris_library_gc (lua_State *L)
{
    ErisLibrary *el = to_eris_library (L);
    TRACE ("%p\n", el);

    if (el->d_globals) {
        dwarf_globals_dealloc (el->d_debug, el->d_globals, el->d_num_globals);
    }

    Dwarf_Error d_error;
    dwarf_finish (el->d_debug, &d_error);

    close (el->fd);
    dlclose (el->dl);

    return 0;
}


static int
eris_library_tostring (lua_State *L)
{
    ErisLibrary *el = to_eris_library (L);
    if (el->d_debug) {
        lua_pushfstring (L, "eris.library (%p)", el->d_debug);
    } else {
        lua_pushliteral (L, "eris.library (closed)");
    }
    return 1;
}


static inline void
eris_symbol_init (ErisSymbol  *symbol,
                  ErisLibrary *library,
                  void        *address,
                  const char  *name)
{
    symbol->library = library;
    symbol->name = strdup (name);
    symbol->address = address;
}


static inline void
eris_symbol_free (ErisSymbol *symbol)
{
    free (symbol->name);
    memset (symbol, 0x00, sizeof (ErisSymbol));
}


static int
make_function_wrapper (lua_State   *L,
                       ErisLibrary *library,
                       void        *address,
                       const char  *name,
                       Dwarf_Die    d_die,
                       Dwarf_Half   d_tag)
{
    ErisFunction *ef = lua_newuserdata (L, sizeof (ErisFunction));
    eris_symbol_init ((ErisSymbol*) ef, library, address, name);
    ef->d_die = d_die;
    luaL_setmetatable (L, ERIS_FUNCTION);
    TRACE ("new ErisFunction* at %p (%p:%s)\n", ef, library, name);
    return 1;
}


static int
make_variable_wrapper (lua_State   *L,
                       ErisLibrary *library,
                       void        *address,
                       const char  *name,
                       Dwarf_Die    d_die,
                       Dwarf_Half   d_tag)
{
    ErisVariable *ev = lua_newuserdata(L, sizeof (ErisVariable));
    eris_symbol_init ((ErisSymbol*) ev, library, address, name);
    luaL_setmetatable (L, ERIS_VARIABLE);
    TRACE ("new ErisVariable* at %p (%p:%s)\n", ev, library, name);
    /* The DIE is unneeded from this point onwards. */
    dwarf_dealloc (library->d_debug, d_die, DW_DLA_DIE);
    return 1;
}


static int
eris_library_lookup (lua_State *L)
{
    ErisLibrary *e = to_eris_library (L);
    const char *name = luaL_checkstring (L, 2);
    const char *error = "unknown error";

    /* Find the entry point of the function. */
    void *address = dlsym (e->dl, name);
    if (!address) {
        error = dlerror ();
        goto return_error;
    }

    Dwarf_Die d_die = lookup_die (e, address, name);
    if (!d_die) {
        return luaL_error (L, "could not look up DWARF debug information "
                           "for symbol '%s' (library %p)", name, e);
    }

    Dwarf_Half d_tag;
    Dwarf_Error d_error = 0;
    if (dwarf_tag (d_die,
                   &d_tag,
                   &d_error) != DW_DLV_OK) {
        return luaL_error (L, "could not obtain DWARF debug information tag "
                           "for symbol '%s' (library %p)", name, e);
    }

    switch (d_tag) {
        case DW_TAG_reference_type:
            /* TODO: Implement dereferencing of references. */
            return luaL_error (L, "DW_TAG_reference_type: unimplemented");

        case DW_TAG_inlined_subroutine: /* TODO: Check whether inlines work. */
        case DW_TAG_entry_point:
        case DW_TAG_subprogram:
            return make_function_wrapper (L, e, address, name, d_die, d_tag);

        case DW_TAG_variable:
            return make_variable_wrapper (L, e, address, name, d_die, d_tag);

        default:
            error = "unsupported debug info kind (not function or data)";
            /* fall-through */
    }

    dwarf_dealloc (e->d_debug, d_die, DW_DLA_DIE);
return_error:
    lua_pushnil (L);
    lua_pushstring (L, error);
    return 2;
}


static int
eris_function_call (lua_State *L)
{
    ErisFunction *ef = to_eris_function (L);
    TRACE ("%p (%p:%s)\n", ef, ef->library, ef->name);
    return 0;
}


static int
eris_function_gc (lua_State *L)
{
    ErisFunction *ef = to_eris_function (L);
    TRACE ("%p (%p:%s)\n", ef, ef->library, ef->name);

    dwarf_dealloc (ef->library->d_debug, ef->d_die, DW_DLA_DIE);
    ef->d_die = 0;

    eris_symbol_free ((ErisSymbol*) ef);
    return 0;
}


static int
eris_function_tostring (lua_State *L)
{
    ErisFunction *ef = to_eris_function (L);
    lua_pushfstring (L, "eris.function (%p:%s)", ef->library, ef->name);
    return 1;
}


/* Methods for ErisLibrary userdatas. */
static const luaL_Reg eris_library_methods[] = {
    { "__gc",       eris_library_gc       },
    { "__tostring", eris_library_tostring },
    { "lookup",     eris_library_lookup   },
    { NULL, NULL }
};


/* Methods for ErisFunction userdatas. */
static const luaL_Reg eris_function_methods[] = {
    { "__call",     eris_function_call     },
    { "__gc",       eris_function_gc       },
    { "__tostring", eris_function_tostring },
    { NULL, NULL }
};


/* Methods for ErisVariable userdatas. */
static int
eris_variable_gc (lua_State *L)
{
    ErisVariable *ev = to_eris_variable (L);
    TRACE ("%p (%p:%s)\n", ev, ev->library, ev->name);
    eris_symbol_free ((ErisSymbol*) ev);
    return 0;
}

static int
eris_variable_tostring (lua_State *L)
{
    ErisVariable *ev = to_eris_variable (L);
    lua_pushfstring (L, "eris.variable (%p:%s)", ev->library, ev->name);
    return 1;
}

static const luaL_Reg eris_variable_methods[] = {
    { "__gc",       eris_variable_gc       },
    { "__tostring", eris_variable_tostring },
    { NULL, NULL },
};


static void
create_meta (lua_State *L)
{
    /* ErisLibrary */
    luaL_newmetatable (L, ERIS_LIBRARY);
    lua_pushvalue (L, -1);           /* Push metatable */
    lua_setfield (L, -2, "__index"); /* metatable.__index == metatable */
    luaL_setfuncs (L, eris_library_methods, 0);
    lua_pop (L, 1);

    /* ErisFunction */
    luaL_newmetatable (L, ERIS_FUNCTION);
    luaL_setfuncs (L, eris_function_methods, 0);
    lua_pop (L, 1);

    /* ErisVariable */
    luaL_newmetatable (L, ERIS_VARIABLE);
    luaL_setfuncs (L, eris_variable_methods, 0);
    lua_pop (L, 1);
}


static int
eris_load (lua_State *L)
{
    size_t name_length;
    const char *name = luaL_checklstring (L, 1, &name_length);
    char path[PATH_MAX];

    errno = 0;
    if (!find_library (name, path)) {
        return luaL_error (L, "could not find library '%s' (%s)", name,
                           (errno == 0) ? "path too long" : strerror (errno));
    }
    TRACE ("found %s -> %s\n", name, path);

    void *dl = dlopen (path, RTLD_NOW | RTLD_GLOBAL);
    if (!dl) {
        return luaL_error (L, "could not link library '%s' (%s)",
                           path, dlerror ());
    }

    int fd = open (path, O_RDONLY, 0);
    if (fd < 0) {
        dlclose (dl);
        return luaL_error (L, "could not open '%s' for reading (%s)",
                           path, strerror (errno));
    }

    Dwarf_Handler d_error_handler = 0;
    Dwarf_Ptr d_error_argument = 0;
    Dwarf_Error d_error;
    Dwarf_Debug d_debug;

    if (dwarf_init (fd,
                    DW_DLC_READ,
                    d_error_handler,
                    d_error_argument,
                    &d_debug,
                    &d_error) != DW_DLV_OK) {
        close (fd);
        dlclose (dl);
        return luaL_error (L, "error reading debug information from '%s' (%s)",
                           path, dwarf_errmsg (d_error));
    }

    Dwarf_Signed d_num_globals = 0;
    Dwarf_Global *d_globals = NULL;
    if (dwarf_get_globals (d_debug,
                           &d_globals,
                           &d_num_globals,
                           &d_error) != DW_DLV_OK) {
        dwarf_finish (d_debug, &d_error);
        close (fd);
        dlclose (dl);
        /* TODO: Provide a better error message. */
        return luaL_error (L, "cannot read globals");
    }
    TRACE ("found %ld globals\n", (long) d_num_globals);
#if defined(ERIS_TRACE) && ERIS_TRACE > 0
    for (Dwarf_Signed i = 0; i < d_num_globals; i++) {
        char *name = NULL;
        if (dwarf_globname (d_globals[i], &name, &d_error) == DW_DLV_OK) {
            TRACE ("-- %s\n", name);
            dwarf_dealloc (d_debug, name, DW_DLA_STRING);
            name = NULL;
        }
    }
#endif /* ERIS_TRACE > 0 */

    ErisLibrary *el = lua_newuserdata (L, sizeof (ErisLibrary));
    el->fd = fd;
    el->dl = dl;
    el->d_debug = d_debug;
    el->d_globals = d_globals;
    el->d_num_globals = d_num_globals;

    if (!find_library_base_address (el)) {
        dwarf_finish (d_debug, &d_error);
        close (fd);
        dlclose (dl);
#ifdef ERIS_USE_LINK_H
        return luaL_error (L, "cannot determine library load address (%s)",
                           dlerror ());
#else
        return luaL_error (L, "cannot determine library load address");
#endif /* ERIS_USE_LINK_H */
    }

    luaL_setmetatable (L, ERIS_LIBRARY);
    TRACE ("new ErisLibrary* at %p\n", el);
    return 1;
}


static const luaL_Reg erislib[] = {
    { "load", eris_load },
    { NULL, NULL },
};


LUAMOD_API int
luaopen_eris (lua_State *L)
{
    eris_trace_setup ();

    (void) elf_version (EV_NONE);
    if (elf_version (EV_CURRENT) == EV_NONE)
        return luaL_error (L, "outdated libelf version");

    luaL_newlib (L, erislib);
    create_meta (L);
    return 1;
}


#ifdef ERIS_USE_LINK_H
# include <link.h>

static bool
find_library_base_address (ErisLibrary *el)
{
    struct link_map *map = NULL;
    if (dlinfo (el->dl, RTLD_DI_LINKMAP, &map) != 0) {
        return false;
    }
    el->dl_diff = map->l_addr;
    TRACE ("diff = %#llx (link_map)\n", (long long) el->dl_diff);
    return true;
}

#else
# error No method chosen to determe the load address of libraries
#endif /* ERIS_USE_LINK_H */


static Dwarf_Die
lookup_die (ErisLibrary *el,
            const void  *address,
            const char  *name)
{
    /*
     * TODO: This performs a linear search. Try to find an alternative way,
     * e.g. using the (optional) DWARF information that correlates entry point
     * addresses with their corresponding DIEs.
     */
    for (Dwarf_Signed i = 0; i < el->d_num_globals; i++) {
        char *global_name;
        Dwarf_Error d_error;
        if (dwarf_globname (el->d_globals[i],
                            &global_name,
                            &d_error) != DW_DLV_OK) {
            TRACE ("skipped malformed global at index %i\n", (int) i);
            continue;
        }

        const bool found = (strcmp (global_name, name) == 0);
        dwarf_dealloc (el->d_debug, global_name, DW_DLA_STRING);
        global_name = NULL;

        if (found) {
            Dwarf_Error d_error;
            Dwarf_Off d_offset;

            if (dwarf_global_die_offset (el->d_globals[i],
                                         &d_offset,
                                         &d_error) != DW_DLV_OK) {
                /* TODO: Print Dwarf_Error to trace log. */
                TRACE ("could not obtain DIE offset\n");
                return NULL;
            }

            Dwarf_Die d_die;
            if (dwarf_offdie (el->d_debug,
                              d_offset,
                              &d_die,
                              &d_error) != DW_DLV_OK) {
                /* TODO: Print Dwarf_Error to trace log. */
                TRACE ("could not obtain DIE\n");
                return NULL;
            }
            return d_die;
        }
    }

    return NULL;
}
