/*
 * screen.c - screen management
 *
 * Copyright © 2007-2008 Julien Danjou <julien@danjou.info>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 */

#include <stdio.h>

#include <xcb/xcb.h>
#include <xcb/xinerama.h>

#include "screen.h"
#include "ewmh.h"
#include "tag.h"
#include "client.h"
#include "widget.h"
#include "wibox.h"
#include "common/xutil.h"

static inline area_t
screen_xsitoarea(xcb_xinerama_screen_info_t si)
{
    area_t a =
    {
        .x = si.x_org,
        .y = si.y_org,
        .width = si.width,
        .height = si.height
    };
    return a;
}

/** Get screens informations and fill global configuration.
 */
void
screen_scan(void)
{
    /* Check for extension before checking for Xinerama */
    if(xcb_get_extension_data(globalconf.connection, &xcb_xinerama_id)->present)
    {
        xcb_xinerama_is_active_reply_t *xia;
        xia = xcb_xinerama_is_active_reply(globalconf.connection, xcb_xinerama_is_active(globalconf.connection), NULL);
        globalconf.xinerama_is_active = xia->state;
        p_delete(&xia);
    }

    if(globalconf.xinerama_is_active)
    {
        xcb_xinerama_query_screens_reply_t *xsq;
        xcb_xinerama_screen_info_t *xsi;
        int xinerama_screen_number;

        xsq = xcb_xinerama_query_screens_reply(globalconf.connection,
                                               xcb_xinerama_query_screens_unchecked(globalconf.connection),
                                               NULL);

        xsi = xcb_xinerama_query_screens_screen_info(xsq);
        xinerama_screen_number = xcb_xinerama_query_screens_screen_info_length(xsq);

        /* now check if screens overlaps (same x,y): if so, we take only the biggest one */
        for(int screen = 0; screen < xinerama_screen_number; screen++)
        {
            bool drop = false;
            foreach(screen_to_test, globalconf.screens)
                if(xsi[screen].x_org == screen_to_test->geometry.x
                   && xsi[screen].y_org == screen_to_test->geometry.y)
                    {
                        /* we already have a screen for this area, just check if
                         * it's not bigger and drop it */
                        drop = true;
                        screen_to_test->geometry.width =
                            MAX(xsi[screen].width, xsi[screen_to_test->index].width);
                        screen_to_test->geometry.height =
                            MAX(xsi[screen].height, xsi[screen_to_test->index].height);
                    }
            if(!drop)
            {
                screen_t s;
                p_clear(&s, 1);
                s.index = screen;
                s.geometry = screen_xsitoarea(xsi[screen]);
                screen_array_append(&globalconf.screens, s);
            }
        }

        p_delete(&xsq);
    }
    else
        /* One screen only / Zaphod mode */
        for(int screen = 0;
            screen < xcb_setup_roots_length(xcb_get_setup(globalconf.connection));
            screen++)
        {
            xcb_screen_t *xcb_screen = xutil_screen_get(globalconf.connection, screen);
            screen_t s;
            p_clear(&s, 1);
            s.index = screen;
            s.geometry.x = 0;
            s.geometry.y = 0;
            s.geometry.width = xcb_screen->width_in_pixels;
            s.geometry.height = xcb_screen->height_in_pixels;
            screen_array_append(&globalconf.screens, s);
        }

    globalconf.screen_focus = globalconf.screens.tab;
}

/** Return the Xinerama screen number where the coordinates belongs to.
 * \param screen The logical screen number.
 * \param x X coordinate
 * \param y Y coordinate
 * \return Screen pointer or screen param if no match or no multi-head.
 */
screen_t *
screen_getbycoord(screen_t *screen, int x, int y)
{
    /* don't waste our time */
    if(!globalconf.xinerama_is_active)
        return screen;

    foreach(s, globalconf.screens)
        if((x < 0 || (x >= s->geometry.x && x < s->geometry.x + s->geometry.width))
           && (y < 0 || (y >= s->geometry.y && y < s->geometry.y + s->geometry.height)))
            return s;

    return screen;
}

/** Get screens info.
 * \param screen Screen.
 * \param wiboxes Wiboxes list to remove.
 * \param padding Padding.
 * \param strut Honor windows strut.
 * \return The screen area.
 */
area_t
screen_area_get(screen_t *screen, wibox_array_t *wiboxes,
                padding_t *padding, bool strut)
{
    area_t area = screen->geometry;
    uint16_t top = 0, bottom = 0, left = 0, right = 0;

    /* make padding corrections */
    if(padding)
    {
        area.x += padding->left;
        area.y += padding->top;
        area.width -= padding->left + padding->right;
        area.height -= padding->top + padding->bottom;
    }

    /* Struts are additive, to allow for multiple clients at the screen edge. */
    /* Some clients request more space than their size, because another window of the same app already has some space. */
    /* So we clamp the strut size. */
    if(strut)
        foreach(_c, globalconf.clients)
        {
            client_t *c = *_c;
            if(client_isvisible(c, screen) && !c->ignore_strut)
            {
                if(c->strut.top_start_x || c->strut.top_end_x)
                {
                    if(c->strut.top)
                        top += MIN(c->strut.top, c->geometry.height);
                    else
                        top += c->geometry.height;
                }
                if(c->strut.bottom_start_x || c->strut.bottom_end_x)
                {
                    if(c->strut.bottom)
                        bottom += MIN(c->strut.bottom, c->geometry.height);
                    else
                        bottom += c->geometry.height;
                }
                if(c->strut.left_start_y || c->strut.left_end_y)
                {
                    if(c->strut.left)
                        left += MIN(c->strut.left, c->geometry.width);
                    else
                        left += c->geometry.width;
                }
                if(c->strut.right_start_y || c->strut.right_end_y)
                {
                    if(c->strut.right)
                        right += MIN(c->strut.right, c->geometry.width);
                    else
                        right += c->geometry.width;
                }
            }
        }

    /* swindow geometry includes borders. */
    if(wiboxes)
        foreach(_w, *wiboxes)
        {
            wibox_t *w = *_w;
            if(w->isvisible)
                switch(w->position)
                {
                  case Top:
                    top += w->sw.geometry.height;
                    break;
                  case Bottom:
                    bottom += w->sw.geometry.height;
                    break;
                  case Left:
                    left += w->sw.geometry.width;
                    break;
                  case Right:
                    right += w->sw.geometry.width;
                    break;
                  default:
                    break;
                }
        }

    area.x += left;
    area.y += top;
    area.width -= left + right;
    area.height -= top + bottom;

    return area;
}

/** Get display info.
 * \param phys_screen Physical screen number.
 * \param wiboxes The wiboxes.
 * \param padding Padding.
 * \return The display area.
 */
area_t
display_area_get(int phys_screen, wibox_array_t *wiboxes, padding_t *padding)
{
    xcb_screen_t *s = xutil_screen_get(globalconf.connection, phys_screen);
    area_t area = { .x = 0,
                    .y = 0,
                    .width = s->width_in_pixels,
                    .height = s->height_in_pixels };

    if(wiboxes)
        foreach(_w, *wiboxes)
        {
            wibox_t *w = *_w;
            area.y += w->position == Top ? w->sw.geometry.height : 0;
            area.height -= (w->position == Top || w->position == Bottom) ? w->sw.geometry.height : 0;
        }

    /* make padding corrections */
    if(padding)
    {
            area.x += padding->left;
            area.y += padding->top;
            area.width -= padding->left + padding->right;
            area.height -= padding->top + padding->bottom;
    }
    return area;
}

/** This returns the real X screen number for a logical
 * screen if Xinerama is active.
 * \param screen The logical screen.
 * \return The X screen.
 */
int
screen_virttophys(int screen)
{
    if(globalconf.xinerama_is_active)
        return globalconf.default_screen;
    return screen;
}

/** Move a client to a virtual screen.
 * \param c The client to move.
 * \param new_screen The destinatiuon screen.
 * \param dotag Set to true if we also change tags.
 * \param doresize Set to true if we also move the client to the new x and
 *        y of the new screen.
 */
void
screen_client_moveto(client_t *c, screen_t *new_screen, bool dotag, bool doresize)
{
    int i;
    screen_t *old_screen = c->screen;
    tag_array_t *old_tags = &old_screen->tags,
                *new_tags = &new_screen->tags;
    area_t from, to;
    bool wasvisible = client_isvisible(c, c->screen);

    if(new_screen == c->screen)
        return;

    c->screen = new_screen;

    if(c->titlebar)
        c->titlebar->screen = new_screen;

    if(dotag && !c->issticky)
    {
        /* remove old tags */
        for(i = 0; i < old_tags->len; i++)
            untag_client(c, old_tags->tab[i]);

        /* add new tags */
        foreach(new_tag, *new_tags)
            if((*new_tag)->selected)
            {
                tag_push(globalconf.L, *new_tag);
                tag_client(c);
            }
    }

    if(wasvisible)
        old_screen->need_arrange = true;
    client_need_arrange(c);

    if(!doresize)
        return;

    from = screen_area_get(old_screen, NULL, NULL, false);
    to = screen_area_get(c->screen, NULL, NULL, false);

    area_t new_geometry = c->geometry;

    if(c->isfullscreen)
    {
        new_geometry = to;
        area_t new_f_geometry = c->geometries.fullscreen;

        new_f_geometry.x = to.x + new_f_geometry.x - from.x;
        new_f_geometry.y = to.y + new_f_geometry.y - from.x;

        /* resize the client's original geometry if it doesn't fit the screen */
        if(new_f_geometry.width > to.width)
            new_f_geometry.width = to.width;
        if(new_f_geometry.height > to.height)
            new_f_geometry.height = to.height;

        /* make sure the client is still on the screen */
        if(new_f_geometry.x + new_f_geometry.width > to.x + to.width)
            new_f_geometry.x = to.x + to.width - new_f_geometry.width;
        if(new_f_geometry.y + new_f_geometry.height > to.y + to.height)
            new_f_geometry.y = to.y + to.height - new_f_geometry.height;

        c->geometries.fullscreen = new_f_geometry;
    }
    else
    {
        new_geometry.x = to.x + new_geometry.x - from.x;
        new_geometry.y = to.y + new_geometry.y - from.y;

        /* resize the client if it doesn't fit the new screen */
        if(new_geometry.width > to.width)
           new_geometry.width = to.width;
        if(new_geometry.height > to.height)
           new_geometry.height = to.height;

        /* make sure the client is still on the screen */
        if(new_geometry.x + new_geometry.width > to.x + to.width)
           new_geometry.x = to.x + to.width - new_geometry.width;
        if(new_geometry.y + new_geometry.height > to.y + to.height)
           new_geometry.y = to.y + to.height - new_geometry.height;
    }
    /* move / resize the client */
    client_resize(c, new_geometry, false);
}

/** Screen module.
 * \param L The Lua VM state.
 * \return The number of elements pushed on stack.
 * \luastack
 * \lfield number The screen number, to get a screen.
 */
static int
luaA_screen_module_index(lua_State *L)
{
    int screen = luaL_checknumber(L, 2) - 1;

    luaA_checkscreen(screen);
    lua_pushlightuserdata(L, &globalconf.screens.tab[screen]);
    return luaA_settype(L, "screen");
}

/** Get or set screen tags.
 * \param L The Lua VM state.
 * \return The number of elements pushed on stack.
 * \luastack
 * \lparam None or a table of tags to set to the screen.
 * The table must contains at least one tag.
 * \return A table with all screen tags.
 */
static int
luaA_screen_tags(lua_State *L)
{
    int i;
    screen_t *s = lua_touserdata(L, 1);

    if(!s)
        luaL_typerror(L, 1, "screen");

    if(lua_gettop(L) == 2)
    {
        luaA_checktable(L, 2);

        /* remove current tags */
        for(i = 0; i < s->tags.len; i++)
            s->tags.tab[i]->screen = NULL;

        tag_array_wipe(&s->tags);
        tag_array_init(&s->tags);

        s->need_arrange = true;

        /* push new tags */
        lua_pushnil(L);
        while(lua_next(L, 2))
            tag_append_to_screen(s);
    }
    else
    {
        lua_newtable(L);
        for(i = 0; i < s->tags.len; i++)
        {
            tag_push(L, s->tags.tab[i]);
            lua_rawseti(L, -2, i + 1);
        }
    }

    return 1;
}

/** A screen.
 * \param L The Lua VM state.
 * \return The number of elements pushed on stack.
 * \luastack
 * \lfield coords The screen coordinates. Immutable.
 * \lfield workarea The screen workarea, i.e. without wiboxes.
 */
static int
luaA_screen_index(lua_State *L)
{
    size_t len;
    const char *buf;
    screen_t *s;

    if(luaA_usemetatable(L, 1, 2))
        return 1;

    buf = luaL_checklstring(L, 2, &len);
    s = lua_touserdata(L, 1);

    switch(a_tokenize(buf, len))
    {
      case A_TK_GEOMETRY:
        luaA_pusharea(L, s->geometry);
        break;
      case A_TK_WORKAREA:
        luaA_pusharea(L, screen_area_get(s, &s->wiboxes, &s->padding, true));
        break;
      default:
        return 0;
    }

    return 1;
}

/** Set or get the screen padding.
 * \param L The Lua VM state.
 * \return The number of elements pushed on stack.
 * \luastack
 * \lparam None or a table with new padding values.
 * \lreturn The screen padding. A table with top, right, left and bottom
 * keys and values in pixel.
 */
static int
luaA_screen_padding(lua_State *L)
{
    screen_t *s = lua_touserdata(L, 1);

    if(!s)
        luaL_typerror(L, 1, "screen");

    if(lua_gettop(L) == 2)
    {
        s->padding = luaA_getopt_padding(L, 2, &s->padding);

        s->need_arrange = true;

        /* All the wiboxes repositioned */
        foreach(w, s->wiboxes)
            wibox_position_update(*w);

        ewmh_update_workarea(screen_virttophys(s->index));
    }
    return luaA_pushpadding(L, &s->padding);
}

/** Get the screen count.
 * \param L The Lua VM state.
 * \return The number of elements pushed on stack.
 *
 * \luastack
 * \lreturn The screen count, at least 1.
 */
static int
luaA_screen_count(lua_State *L)
{
    lua_pushnumber(L, globalconf.screens.len);
    return 1;
}

const struct luaL_reg awesome_screen_methods[] =
{
    { "count", luaA_screen_count },
    { "__index", luaA_screen_module_index },
    { NULL, NULL }
};

const struct luaL_reg awesome_screen_meta[] =
{
    { "tags", luaA_screen_tags },
    { "padding", luaA_screen_padding },
    { "__index", luaA_screen_index },
    { NULL, NULL }
};

// vim: filetype=c:expandtab:shiftwidth=4:tabstop=8:softtabstop=4:encoding=utf-8:textwidth=80
