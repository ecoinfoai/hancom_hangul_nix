/* SPDX-License-Identifier: MIT */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <poll.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <xcb/xcb.h>

typedef xcb_intern_atom_cookie_t (*intern_atom_fn)(
    xcb_connection_t *, uint8_t, uint16_t, const char *);
typedef xcb_intern_atom_reply_t *(*intern_atom_reply_fn)(
    xcb_connection_t *, xcb_intern_atom_cookie_t, xcb_generic_error_t **);
typedef int (*conn_has_error_fn)(xcb_connection_t *);

xcb_intern_atom_cookie_t xcb_intern_atom(
    xcb_connection_t *c, uint8_t only_if_exists,
    uint16_t name_len, const char *name)
{
    static intern_atom_fn real;
    static conn_has_error_fn has_err;
    if (!real) {
        real    = (intern_atom_fn)dlsym(RTLD_NEXT, "xcb_intern_atom");
        has_err = (conn_has_error_fn)dlsym(RTLD_NEXT, "xcb_connection_has_error");
    }
    if (!c || (has_err && has_err(c))) {
        xcb_intern_atom_cookie_t dummy;
        memset(&dummy, 0, sizeof(dummy));
        return dummy;
    }
    return real(c, only_if_exists, name_len, name);
}

xcb_intern_atom_reply_t *xcb_intern_atom_reply(
    xcb_connection_t *c, xcb_intern_atom_cookie_t cookie,
    xcb_generic_error_t **e)
{
    static intern_atom_reply_fn real;
    static conn_has_error_fn has_err;
    if (!real) {
        real    = (intern_atom_reply_fn)dlsym(RTLD_NEXT, "xcb_intern_atom_reply");
        has_err = (conn_has_error_fn)dlsym(RTLD_NEXT, "xcb_connection_has_error");
    }
    if (!c || (has_err && has_err(c)) || cookie.sequence == 0) {
        if (e) *e = NULL;
        return NULL;
    }
    return real(c, cookie, e);
}

/*
 * Redirect HWP's PRIMARY reads to CLIPBOARD.
 *
 * HWP's Linux build pastes from QClipboard::Selection (PRIMARY), not
 * CLIPBOARD, and polls it ~75 times a second. Pointing those reads at
 * CLIPBOARD makes Ctrl+V paste what was copied. HWP sets both selections
 * itself on copy, so nothing changes on the copy side.
 *
 * Qt 5.11 never caches an empty TARGETS answer and sleeps 50 ms per retry, so
 * an owner that refuses or ignores conversions freezes HWP until its watchdog
 * reports "unexpected problem". Mutter leaves X selections in that refusing
 * state after the Wayland selection is cleared. Such owners are reported as
 * absent, judged by a TARGETS probe on a private connection. Qt 5.15 does not
 * retry this way.
 *
 * Qt drops its copy of the PRIMARY data on SelectionClear, which would make
 * HWP lose its own copy as soon as text is selected elsewhere, so that event
 * is swallowed. XFixes CLIPBOARD changes are relabelled as PRIMARY so Qt
 * refreshes the format list it caches for Selection mode.
 */
#define PROBE_TIMEOUT_MS 200
#define PROBE_TTL_MS 500
#define XFIXES_SELECTION_NOTIFY 0
#define PENDING 8

static pthread_once_t init_once = PTHREAD_ONCE_INIT;
static xcb_connection_t *probe;
static xcb_window_t probe_win;
static xcb_atom_t clipboard, targets, probe_prop;
static int xfixes_event = -1;
/* Set once init() has filled in the atoms above; read by Qt's event thread. */
static int ready;

static xcb_atom_t intern(const char *name)
{
    xcb_intern_atom_reply_t *r = xcb_intern_atom_reply(
        probe, xcb_intern_atom(probe, 0, strlen(name), name), NULL);
    xcb_atom_t a = r->atom;
    free(r);
    return a;
}

static void init(void)
{
    probe = xcb_connect(NULL, NULL);
    if (xcb_connection_has_error(probe)) {
        fprintf(stderr, "xcb_shim: cannot open the selection probe connection\n");
        abort();
    }
    clipboard = intern("CLIPBOARD");
    targets = intern("TARGETS");
    probe_prop = intern("_XCB_SHIM_PROBE");
    xcb_query_extension_reply_t *ext = xcb_query_extension_reply(
        probe, xcb_query_extension(probe, 6, "XFIXES"), NULL);
    if (ext->present) xfixes_event = ext->first_event + XFIXES_SELECTION_NOTIFY;
    free(ext);
    xcb_screen_t *s = xcb_setup_roots_iterator(xcb_get_setup(probe)).data;
    probe_win = xcb_generate_id(probe);
    xcb_create_window(probe, XCB_COPY_FROM_PARENT, probe_win, s->root, 0, 0, 1, 1, 0,
                      XCB_WINDOW_CLASS_INPUT_ONLY, s->root_visual, 0, NULL);
    __atomic_store_n(&ready, 1, __ATOMIC_RELEASE);
}

static long now_ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1000L + t.tv_nsec / 1000000L;
}

/* Whether the foreign CLIPBOARD owner answers TARGETS; cached briefly because
 * HWP polls far faster than the clipboard changes. */
static int clipboard_answers(xcb_window_t owner)
{
    static xcb_window_t cached_owner;
    static long cached_at;
    static int cached;
    long now = now_ms();
    if (owner == cached_owner && now - cached_at < PROBE_TTL_MS) return cached;

    xcb_generic_event_t *e;
    while ((e = xcb_poll_for_event(probe))) free(e);
    xcb_convert_selection(probe, probe_win, clipboard, targets, probe_prop, XCB_CURRENT_TIME);
    xcb_flush(probe);

    int answered = 0;
    long deadline = now + PROBE_TIMEOUT_MS;
    for (long left = PROBE_TIMEOUT_MS; left > 0; left = deadline - now_ms()) {
        struct pollfd p = { xcb_get_file_descriptor(probe), POLLIN, 0 };
        poll(&p, 1, (int)left);
        while ((e = xcb_poll_for_event(probe))) {
            xcb_selection_notify_event_t *n = (xcb_selection_notify_event_t *)e;
            int mine = (e->response_type & 0x7f) == XCB_SELECTION_NOTIFY
                && n->requestor == probe_win;
            if (mine) answered = n->property != XCB_NONE;
            free(e);
            if (mine) goto done;
        }
    }
done:
    cached_owner = owner;
    cached_at = now_ms();
    cached = answered;
    return answered;
}

/* Qt issues selection queries from its GUI thread only, so no locking. */
static struct { xcb_connection_t *c; unsigned int sequence; } redirected[PENDING];
static unsigned int next_redirect;

typedef xcb_get_selection_owner_cookie_t (*get_owner_fn)(xcb_connection_t *, xcb_atom_t);
typedef xcb_get_selection_owner_reply_t *(*get_owner_reply_fn)(
    xcb_connection_t *, xcb_get_selection_owner_cookie_t, xcb_generic_error_t **);
typedef xcb_void_cookie_t (*convert_fn)(
    xcb_connection_t *, xcb_window_t, xcb_atom_t, xcb_atom_t, xcb_atom_t, xcb_timestamp_t);
typedef xcb_generic_event_t *(*next_event_fn)(xcb_connection_t *);

static xcb_get_selection_owner_cookie_t query_owner(
    get_owner_fn real, xcb_connection_t *c, xcb_atom_t selection)
{
    if (selection != XCB_ATOM_PRIMARY) return real(c, selection);
    pthread_once(&init_once, init);
    xcb_get_selection_owner_cookie_t cookie = real(c, clipboard);
    redirected[next_redirect % PENDING].c = c;
    redirected[next_redirect % PENDING].sequence = cookie.sequence;
    next_redirect++;
    return cookie;
}

xcb_get_selection_owner_cookie_t xcb_get_selection_owner(xcb_connection_t *c, xcb_atom_t selection)
{
    static get_owner_fn real;
    if (!real) real = (get_owner_fn)dlsym(RTLD_NEXT, "xcb_get_selection_owner");
    return query_owner(real, c, selection);
}

xcb_get_selection_owner_cookie_t xcb_get_selection_owner_unchecked(xcb_connection_t *c, xcb_atom_t selection)
{
    static get_owner_fn real;
    if (!real) real = (get_owner_fn)dlsym(RTLD_NEXT, "xcb_get_selection_owner_unchecked");
    return query_owner(real, c, selection);
}

xcb_get_selection_owner_reply_t *xcb_get_selection_owner_reply(
    xcb_connection_t *c, xcb_get_selection_owner_cookie_t cookie, xcb_generic_error_t **e)
{
    static get_owner_reply_fn real;
    if (!real) real = (get_owner_reply_fn)dlsym(RTLD_NEXT, "xcb_get_selection_owner_reply");
    xcb_get_selection_owner_reply_t *r = real(c, cookie, e);
    if (!r) return r;
    for (int i = 0; i < PENDING; i++) {
        if (redirected[i].c != c || redirected[i].sequence != cookie.sequence) continue;
        redirected[i].c = NULL;
        const xcb_setup_t *s = xcb_get_setup(c);
        int own = (r->owner & ~s->resource_id_mask) == s->resource_id_base;
        if (r->owner != XCB_NONE && !own && !clipboard_answers(r->owner))
            r->owner = XCB_NONE;
        break;
    }
    return r;
}

xcb_void_cookie_t xcb_convert_selection(
    xcb_connection_t *c, xcb_window_t requestor, xcb_atom_t selection,
    xcb_atom_t target, xcb_atom_t property, xcb_timestamp_t time)
{
    static convert_fn real;
    if (!real) real = (convert_fn)dlsym(RTLD_NEXT, "xcb_convert_selection");
    if (selection == XCB_ATOM_PRIMARY) {
        pthread_once(&init_once, init);
        selection = clipboard;
    }
    return real(c, requestor, selection, target, property, time);
}

/* Returns NULL for events HWP must not see; the caller fetches the next one.
 *
 * Never initializes the shim: Qt 5.15 reads events on its own thread, and
 * GTK holds a server grab during gtk_init, so opening the probe connection
 * here would block forever. Before the first PRIMARY query nothing needs
 * relabelling, because Qt has no cached Selection formats yet. */
static xcb_generic_event_t *filter(xcb_generic_event_t *e)
{
    if (!e) return e;
    uint8_t type = e->response_type & 0x7f;
    if (type == XCB_SELECTION_CLEAR
            && ((xcb_selection_clear_event_t *)e)->selection == XCB_ATOM_PRIMARY) {
        free(e);
        return NULL;
    }
    if (__atomic_load_n(&ready, __ATOMIC_ACQUIRE) && type == xfixes_event) {
        /* xcb_xfixes_selection_notify_event_t.selection sits at byte 12. */
        uint32_t *selection = (uint32_t *)((char *)e + 12);
        if (*selection == clipboard) *selection = XCB_ATOM_PRIMARY;
    }
    return e;
}

static xcb_generic_event_t *next_event(next_event_fn real, xcb_connection_t *c)
{
    for (;;) {
        xcb_generic_event_t *e = real(c);
        if (!e) return e;
        if ((e = filter(e))) return e;
    }
}

xcb_generic_event_t *xcb_wait_for_event(xcb_connection_t *c)
{
    static next_event_fn real;
    if (!real) real = (next_event_fn)dlsym(RTLD_NEXT, "xcb_wait_for_event");
    return next_event(real, c);
}

xcb_generic_event_t *xcb_poll_for_event(xcb_connection_t *c)
{
    static next_event_fn real;
    if (!real) real = (next_event_fn)dlsym(RTLD_NEXT, "xcb_poll_for_event");
    return next_event(real, c);
}

xcb_generic_event_t *xcb_poll_for_queued_event(xcb_connection_t *c)
{
    static next_event_fn real;
    if (!real) real = (next_event_fn)dlsym(RTLD_NEXT, "xcb_poll_for_queued_event");
    return next_event(real, c);
}
