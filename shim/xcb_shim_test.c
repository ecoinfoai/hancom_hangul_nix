/* SPDX-License-Identifier: MIT */
/* Checks the PRIMARY-to-CLIPBOARD redirect in xcb_shim.so; run under Xvfb
 * with the shim in LD_PRELOAD. "self" plays HWP's Qt connection; selection
 * owners run in child processes so they can answer conversions.
 * Exit status is the number of failed checks. */
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <xcb/xcb.h>
#include <xcb/xfixes.h>

enum owner_mode { ANSWER, REFUSE, SILENT };

static int failures;
static xcb_atom_t clipboard, targets, utf8, prop;

static void check(const char *name, int ok)
{
    printf("%s %s\n", ok ? "ok  " : "FAIL", name);
    failures += !ok;
}

static long now_ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1000L + t.tv_nsec / 1000000L;
}

static xcb_window_t make_window(xcb_connection_t *c)
{
    xcb_screen_t *s = xcb_setup_roots_iterator(xcb_get_setup(c)).data;
    xcb_window_t w = xcb_generate_id(c);
    xcb_create_window(c, XCB_COPY_FROM_PARENT, w, s->root, 0, 0, 1, 1, 0,
                      XCB_WINDOW_CLASS_INPUT_ONLY, s->root_visual, 0, NULL);
    return w;
}

static xcb_atom_t intern(xcb_connection_t *c, const char *name)
{
    xcb_intern_atom_reply_t *r = xcb_intern_atom_reply(
        c, xcb_intern_atom(c, 0, strlen(name), name), NULL);
    xcb_atom_t a = r->atom;
    free(r);
    return a;
}

static void sync_conn(xcb_connection_t *c)
{
    free(xcb_get_input_focus_reply(c, xcb_get_input_focus(c), NULL));
}

static xcb_window_t owner_of(xcb_connection_t *c, xcb_atom_t sel, int unchecked)
{
    xcb_get_selection_owner_cookie_t k = unchecked
        ? xcb_get_selection_owner_unchecked(c, sel) : xcb_get_selection_owner(c, sel);
    xcb_get_selection_owner_reply_t *r = xcb_get_selection_owner_reply(c, k, NULL);
    xcb_window_t w = r->owner;
    free(r);
    return w;
}

/* Next event of the given type within timeout_ms, or NULL. */
static xcb_generic_event_t *next_event(xcb_connection_t *c, uint8_t type, int timeout_ms)
{
    long deadline = now_ms() + timeout_ms;
    for (;;) {
        xcb_generic_event_t *e;
        while ((e = xcb_poll_for_event(c))) {
            if ((e->response_type & 0x7f) == type) return e;
            free(e);
        }
        long left = deadline - now_ms();
        if (left <= 0) return NULL;
        struct pollfd p = { xcb_get_file_descriptor(c), POLLIN, 0 };
        poll(&p, 1, (int)left);
    }
}

static void serve(xcb_connection_t *c, enum owner_mode mode)
{
    xcb_generic_event_t *e;
    while ((e = xcb_wait_for_event(c))) {
        if ((e->response_type & 0x7f) == XCB_SELECTION_REQUEST && mode != SILENT) {
            xcb_selection_request_event_t *rq = (xcb_selection_request_event_t *)e;
            xcb_selection_notify_event_t n = {
                .response_type = XCB_SELECTION_NOTIFY, .time = rq->time,
                .requestor = rq->requestor, .selection = rq->selection,
                .target = rq->target, .property = XCB_NONE };
            if (mode == ANSWER && rq->target == targets) {
                xcb_atom_t list[] = { targets, utf8 };
                xcb_change_property(c, XCB_PROP_MODE_REPLACE, rq->requestor, rq->property,
                                    XCB_ATOM_ATOM, 32, 2, list);
                n.property = rq->property;
            } else if (mode == ANSWER && rq->target == utf8) {
                xcb_change_property(c, XCB_PROP_MODE_REPLACE, rq->requestor, rq->property,
                                    utf8, 8, 4, "CLIP");
                n.property = rq->property;
            }
            xcb_send_event(c, 0, rq->requestor, XCB_EVENT_MASK_NO_EVENT, (const char *)&n);
            xcb_flush(c);
        }
        free(e);
    }
}

/* Fork a client that owns CLIPBOARD and treats conversions per mode. */
static pid_t spawn_clipboard_owner(enum owner_mode mode)
{
    int ready[2];
    if (pipe(ready) != 0) { perror("pipe"); exit(100); }
    pid_t pid = fork();
    if (pid == 0) {
        xcb_connection_t *c = xcb_connect(NULL, NULL);
        xcb_window_t w = make_window(c);
        xcb_set_selection_owner(c, w, clipboard, XCB_CURRENT_TIME);
        sync_conn(c);
        if (write(ready[1], "x", 1) != 1) _exit(1);
        serve(c, mode);
        _exit(0);
    }
    char b;
    if (read(ready[0], &b, 1) != 1) { perror("owner start"); exit(100); }
    close(ready[0]);
    close(ready[1]);
    return pid;
}

/* GTK grabs the server while it looks for the XSETTINGS manager, and Qt 5.15
 * reads events on a second thread meanwhile. Reading an event must not make
 * the shim open its probe connection, which the grab would stall forever.
 * Runs in a child so the shim is still uninitialized. */
static int events_flow_during_grab(void)
{
    pid_t pid = fork();
    if (pid == 0) {
        xcb_connection_t *c = xcb_connect(NULL, NULL);
        xcb_window_t w = make_window(c);
        uint32_t mask = XCB_EVENT_MASK_PROPERTY_CHANGE;
        xcb_change_window_attributes(c, w, XCB_CW_EVENT_MASK, &mask);
        xcb_grab_server(c);
        xcb_change_property(c, XCB_PROP_MODE_REPLACE, w, XCB_ATOM_WM_NAME,
                            XCB_ATOM_STRING, 8, 1, "x");
        sync_conn(c);
        xcb_generic_event_t *e = xcb_poll_for_event(c);
        if (!e) _exit(2);
        free(e);
        xcb_ungrab_server(c);
        sync_conn(c);
        _exit(0);
    }
    for (long deadline = now_ms() + 2000; now_ms() < deadline; usleep(10 * 1000)) {
        int status;
        if (waitpid(pid, &status, WNOHANG) == pid)
            return WIFEXITED(status) && WEXITSTATUS(status) == 0;
    }
    kill(pid, SIGKILL);
    waitpid(pid, NULL, 0);
    return 0;
}

/* The shim caches its owner probe for 500 ms; the next owner may even reuse
 * the same window ID, so let the cache expire between owners. */
static void stop(pid_t pid)
{
    kill(pid, SIGTERM);
    waitpid(pid, NULL, 0);
    usleep(600 * 1000);
}

int main(void)
{
    check("reading events during a server grab does not block",
          events_flow_during_grab());

    xcb_connection_t *self = xcb_connect(NULL, NULL);
    xcb_connection_t *other = xcb_connect(NULL, NULL);
    if (xcb_connection_has_error(self) || xcb_connection_has_error(other)) {
        fprintf(stderr, "cannot connect to X display\n");
        return 100;
    }
    clipboard = intern(self, "CLIPBOARD");
    targets = intern(self, "TARGETS");
    utf8 = intern(self, "UTF8_STRING");
    prop = intern(self, "TEST_SELECTION");
    xcb_window_t self_win = make_window(self);
    xcb_window_t other_win = make_window(other);
    sync_conn(other);

    pid_t owner = spawn_clipboard_owner(ANSWER);
    xcb_window_t owner_win = owner_of(self, clipboard, 0);
    check("PRIMARY owner is the answering CLIPBOARD owner",
          owner_win != XCB_NONE && owner_of(self, XCB_ATOM_PRIMARY, 0) == owner_win);
    check("PRIMARY owner is the answering CLIPBOARD owner (unchecked)",
          owner_of(self, XCB_ATOM_PRIMARY, 1) == owner_win);

    xcb_convert_selection(self, self_win, XCB_ATOM_PRIMARY, utf8, prop, XCB_CURRENT_TIME);
    xcb_flush(self);
    xcb_selection_notify_event_t *n =
        (xcb_selection_notify_event_t *)next_event(self, XCB_SELECTION_NOTIFY, 2000);
    xcb_get_property_reply_t *data = n && n->property != XCB_NONE
        ? xcb_get_property_reply(self, xcb_get_property(self, 1, self_win, prop,
                                                        XCB_GET_PROPERTY_TYPE_ANY, 0, 64), NULL)
        : NULL;
    check("converting PRIMARY yields the CLIPBOARD content",
          data && xcb_get_property_value_length(data) == 4
              && memcmp(xcb_get_property_value(data), "CLIP", 4) == 0);
    free(data);
    free(n);
    stop(owner);

    owner = spawn_clipboard_owner(REFUSE);
    owner_win = owner_of(self, clipboard, 0);
    check("refusing CLIPBOARD owner is reported as no PRIMARY owner",
          owner_of(self, XCB_ATOM_PRIMARY, 0) == XCB_NONE);
    check("CLIPBOARD owner query itself is untouched", owner_win != XCB_NONE);
    stop(owner);

    owner = spawn_clipboard_owner(SILENT);
    long t0 = now_ms();
    xcb_window_t seen = owner_of(self, XCB_ATOM_PRIMARY, 0);
    long took = now_ms() - t0;
    check("silent CLIPBOARD owner is reported as no PRIMARY owner within 1 s",
          seen == XCB_NONE && took < 1000);
    stop(owner);

    xcb_set_selection_owner(self, self_win, clipboard, XCB_CURRENT_TIME);
    check("own CLIPBOARD makes self the PRIMARY owner",
          owner_of(self, XCB_ATOM_PRIMARY, 0) == self_win);

    xcb_set_selection_owner(self, self_win, XCB_ATOM_PRIMARY, XCB_CURRENT_TIME);
    sync_conn(self);
    for (xcb_generic_event_t *e; (e = xcb_poll_for_event(self)); ) free(e);
    xcb_set_selection_owner(other, other_win, XCB_ATOM_PRIMARY, XCB_CURRENT_TIME);
    xcb_set_selection_owner(other, other_win, clipboard, XCB_CURRENT_TIME);
    sync_conn(other);
    xcb_selection_clear_event_t *clear =
        (xcb_selection_clear_event_t *)next_event(self, XCB_SELECTION_CLEAR, 2000);
    check("losing PRIMARY is hidden, losing CLIPBOARD is not",
          clear && clear->selection == clipboard);
    free(clear);

    const xcb_query_extension_reply_t *xf = xcb_get_extension_data(self, &xcb_xfixes_id);
    free(xcb_xfixes_query_version_reply(self, xcb_xfixes_query_version(self, 5, 0), NULL));
    xcb_xfixes_select_selection_input(self, self_win, clipboard,
                                      XCB_XFIXES_SELECTION_EVENT_MASK_SET_SELECTION_OWNER);
    sync_conn(self);
    xcb_set_selection_owner(other, XCB_NONE, clipboard, XCB_CURRENT_TIME);
    xcb_set_selection_owner(other, other_win, clipboard, XCB_CURRENT_TIME);
    sync_conn(other);
    xcb_xfixes_selection_notify_event_t *fx = (xcb_xfixes_selection_notify_event_t *)
        next_event(self, xf->first_event + XCB_XFIXES_SELECTION_NOTIFY, 2000);
    check("CLIPBOARD change is announced as a PRIMARY change",
          fx && fx->selection == XCB_ATOM_PRIMARY);
    free(fx);

    xcb_disconnect(other);
    xcb_disconnect(self);
    return failures;
}
