/* Windower's regular expressions (windower.regex.*): ECMAScript std::regex, as Windower's are.
 * Registered by windower_native.c into xi.windower_native:
 *
 *   regex_find_all(str, pattern) -> { {start, end, capture1, ...}, ... }   (1-based, inclusive;
 *       a capture that didn't take part is false)
 *
 * and, on Windows (no POSIX <regex.h>), the Ashita layer's one-match finder for ashita_native.c:
 *
 *   regex_find(subject, pattern[, init[, icase]]) -> start, stop, { whole, capture1, ... } | nil
 *
 * Patterns are compiled once and cached. A bad pattern raises a Lua error with std::regex's text;
 * no C++ object is alive when it does. */
#include <cstdio>
#include <map>
#include <regex>
#include <string>

extern "C" {
#include "lauxlib.h"
#include "lua.h"
}

static char g_err[300];

/* NULL (g_err set) when the pattern doesn't compile. */
static const std::regex* compile(const char* pattern, bool icase = false)
{
    static std::map<std::string, std::regex> cache;
    try
    {
        std::string key = (icase ? "i:" : "c:") + std::string(pattern);
        auto it = cache.find(key);
        if (it != cache.end())
            return &it->second;
        if (cache.size() > 256)
            cache.clear();
        auto flags = std::regex::ECMAScript | (icase ? std::regex::icase : std::regex::ECMAScript);
        return &cache.emplace(key, std::regex(pattern, flags)).first->second;
    }
    catch (const std::exception& e)
    {
        snprintf(g_err, sizeof g_err, "regex: %s: %s", pattern, e.what());
        return nullptr;
    }
}

/* Pushes the matches table; 0 on failure (g_err set). */
static int find_all(lua_State* L, const char* s, size_t len, const std::regex* re)
{
    try
    {
        lua_newtable(L);
        int n = 0;
        for (std::cregex_iterator it(s, s + len, *re), end; it != end; ++it)
        {
            const std::cmatch& m = *it;
            lua_newtable(L);
            lua_pushnumber(L, (lua_Number)m.position(0) + 1);
            lua_rawseti(L, -2, 1);
            lua_pushnumber(L, (lua_Number)(m.position(0) + m.length(0)));
            lua_rawseti(L, -2, 2);
            for (size_t i = 1; i < m.size(); ++i)
            {
                if (m[i].matched)
                    lua_pushlstring(L, s + m.position(i), (size_t)m.length(i));
                else
                    lua_pushboolean(L, 0);
                lua_rawseti(L, -2, (int)i + 2);
            }
            lua_rawseti(L, -2, ++n);
        }
        return 1;
    }
    catch (const std::exception& e)
    {
        snprintf(g_err, sizeof g_err, "regex: %s", e.what());
        return 0;
    }
}

extern "C" int xi_windower_regex_find_all(lua_State* L)
{
    size_t len;
    const char* s = luaL_checklstring(L, 1, &len);
    const std::regex* re = compile(luaL_checkstring(L, 2));
    if (!re || !find_all(L, s, len, re))
        return luaL_error(L, "%s", g_err);
    return 1;
}

/* Ashita's regex_find: the first match at or after byte `init` (1-based). Returns 1-based inclusive
 * positions and the whole match then each capture ("" for one that didn't take part), or nil. */
extern "C" int xi_regex_find(lua_State* L)
{
    size_t len;
    const char* s = luaL_checklstring(L, 1, &len);
    const char* pattern = luaL_checkstring(L, 2);
    size_t init = (size_t)luaL_optnumber(L, 3, 1);
    bool icase = lua_toboolean(L, 4) != 0;
    if (init < 1)
        init = 1;
    if (init > len + 1)
        return lua_pushnil(L), 1;
    const std::regex* re = compile(pattern, icase);
    if (!re)
        return luaL_error(L, "%s", g_err);
    std::cmatch m;
    bool found;
    try
    {
        // past the start, ^ and \b see the byte before, as they would in the whole subject
        auto flags = init > 1 ? std::regex_constants::match_prev_avail : std::regex_constants::match_default;
        found = std::regex_search(s + init - 1, s + len, m, *re, flags);
    }
    catch (const std::exception& e)
    {
        snprintf(g_err, sizeof g_err, "regex: %s", e.what());
        return luaL_error(L, "%s", g_err);
    }
    if (!found)
        return lua_pushnil(L), 1;
    size_t start = init - 1 + (size_t)m.position(0);
    lua_pushnumber(L, (lua_Number)start + 1);
    lua_pushnumber(L, (lua_Number)(start + (size_t)m.length(0)));
    lua_newtable(L);
    for (size_t i = 0; i < m.size(); ++i)
    {
        if (m[i].matched)
            lua_pushlstring(L, m[i].first, (size_t)m.length(i));
        else
            lua_pushstring(L, "");
        lua_rawseti(L, -2, (int)i + 1);
    }
    return 3;
}
