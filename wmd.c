#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xresource.h>
#include <X11/Xutil.h>

// TODO
// WM_TRANSIENT_FOR https://tronche.com/gui/x/icccm/sec-4.html#WM_TRANSIENT_FOR

/* https://tronche.com/gui/x/xlib/ */
/* https://tronche.com/gui/x/icccm/ */
/* https://specifications.freedesktop.org/wm-spec/wm-spec-latest.html */

#define FLAG_ROOT       "r"
#define FLAG_ACTIVE     "a"
#define FLAG_POINTER    "p"
#define FLAG_FULLSCREEN "f"
#define FLAG_ABOVE      "t"
#define FLAG_URGENT     "u"
#define FLAG_ICONIC     "i"
#define FLAG_COUNT      7
/* #define FLAG_ATTENTION */

enum {
    WM_PROTOCOLS,
    WM_STATE,
    WM_CHANGE_STATE,
    WM_DELETE_WINDOW,
    WM_TAKE_FOCUS,
    wm_atoms_count
};

enum {
    _NET_SUPPORTED,
    _NET_ACTIVE_WINDOW,
    _NET_WM_NAME,
    /* _NET_CLIENT_LIST, */
    _NET_SUPPORTING_WM_CHECK,
    _NET_WM_STATE,
    _NET_WM_STATE_ABOVE,
    _NET_WM_STATE_DEMANDS_ATTENTION,
    /* _NET_MW_STATE_FOCUSED, */
    _NET_WM_STATE_FULLSCREEN,
    _NET_WM_WINDOW_TYPE,
    _NET_WM_WINDOW_TYPE_DIALOG,
    _NET_WM_WINDOW_TYPE_DOCK,
    _NET_WM_WINDOW_TYPE_SPLASH,
    net_atoms_count
};

#define MWM_HINTS_DECORATIONS (1L << 1)

typedef struct {
    unsigned long flags;
    unsigned long functions;
    unsigned long decorations;
    long input_mode;
    unsigned long status;
} MotifWmHints;

enum {
    _NET_WM_STATE_REMOVE,
    _NET_WM_STATE_ADD,
    _NET_WM_STATE_TOGGLE
};

static Bool quit = False;
static Bool restart = False;

/* X */
static Display *display;
static int screen;
static Window root;
static int screen_width;
static int screen_height;
static Atom wm_atoms[wm_atoms_count];
static Atom net_atoms[net_atoms_count];
static Atom _MOTIF_WM_HINTS;

static char *prefix = "W";
static FILE *fifo = NULL;

/* settings */
static unsigned int foreground;
static unsigned int background;
static int border_size;
static int gap_size;
static int top_padding;

static void iconify_window(Window window, Bool iconify);
static void activate_window(Window window);
static void raise_window(Window window);
static void fullscreen_window(Window window);

static int error_handler(Display *display, XErrorEvent *error) {
    return 0;
}

void set_window_property(Window window, Atom property, Window value) {
    XChangeProperty(display, window, property, XA_WINDOW, 32,
                    PropModeReplace, (unsigned char *) &value, 1);
}

unsigned char *get_property(Window window, Atom property, long length, Atom type) {
    Atom actual_type;
    int actual_format;
    unsigned long nitems;
    unsigned long bytes_after;
    unsigned char *prop;

    prop = NULL;
    XGetWindowProperty(display, window, property, 0L, length, False, type,
                       &actual_type, &actual_format, &nitems, &bytes_after, &prop);
    return prop;
}

Atom get_atom_property(Window window, Atom property) {
    unsigned char *prop;
    Atom atom;

    atom = None;
    prop = get_property(window, property, sizeof(atom), XA_ATOM);
    if (prop) {
        atom = *(Atom *)prop;
        XFree(prop);
    }

    return atom;
}

int get_border_size(Window window) {
    Atom actual_type;
    int actual_format;
    unsigned long nitems;
    unsigned long bytes_after;
    unsigned char *prop = NULL;
    MotifWmHints *hints;

    int size = border_size;

    XGetWindowProperty(display, window, _MOTIF_WM_HINTS, 0L, 5, False, _MOTIF_WM_HINTS,
                       &actual_type, &actual_format, &nitems, &bytes_after, &prop);

    if (prop &&
        actual_type == _MOTIF_WM_HINTS &&
        actual_format == 32 &&
        nitems == 5) {
        hints = (MotifWmHints *) prop;
        if (hints->flags & MWM_HINTS_DECORATIONS && hints->decorations == 0) {
            size = 0;
        }
    }

    if (prop) {
        XFree(prop);
    }

    return size;
}

Window get_active_window() {
    unsigned char *prop;
    Window window;

    window = None;
    prop = get_property(root, net_atoms[_NET_ACTIVE_WINDOW], sizeof(window), XA_WINDOW);
    if (prop) {
        window = *(Window *)prop;
        XFree(prop);
    }

    return window;
}

Window get_pointer_window() {
    Window child;
    int root_x;
    int root_y;
    int win_x;
    int win_y;
    unsigned int mask;

    XQueryPointer(display, root, &root, &child, &root_x, &root_y, &win_x, &win_y, &mask);
    return child;
}

void set_wm_state(Window window, long state) {
    long data[] = { state, None };

    XChangeProperty(display, window, wm_atoms[WM_STATE], wm_atoms[WM_STATE], 32,
                    PropModeReplace, (unsigned char *) data, 2);
}

long get_wm_state(Window window) {
    unsigned char *prop = NULL;
    long state = -1;

    prop = get_property(window, wm_atoms[WM_STATE], 2L, wm_atoms[WM_STATE]);
    if (prop) {
        state = *prop;
        XFree(prop);
    }
    return state;
}

void set_net_wm_state(Window window, Atom state, Bool set) {
    Atom actual_type;
    int actual_format;
    unsigned long nitems;
    unsigned long bytes_after;
    Atom *prop;

    prop = NULL;
    if (XGetWindowProperty(
            display, window, net_atoms[_NET_WM_STATE], 0L, ~0L, False, XA_ATOM,
            &actual_type, &actual_format, &nitems, &bytes_after, (unsigned char **) &prop) == Success &&
        actual_type == XA_ATOM && actual_format == 32 && prop) {
        Atom *states = malloc((nitems + (set ? 1 : 0)) * sizeof(Atom));
        int nstates = 0;
        unsigned long i;
        for (i = 0; i < nitems; i++) {
            if (prop[i] == state) {
                if (set) {
                    break;
                }
            } else {
                states[nstates++] = prop[i];
            }
        }
        if (i == nitems) {
            if (set) {
                states[nstates++] = state;
            }
            if (nstates) {
                XChangeProperty(display, window, net_atoms[_NET_WM_STATE], XA_ATOM, 32,
                                PropModeReplace, (unsigned char *) states, nstates);
            } else {
                XDeleteProperty(display, window, net_atoms[_NET_WM_STATE]);
            }
        }
        free(states);
    } else if (set) {
        XChangeProperty(display, window, net_atoms[_NET_WM_STATE], XA_ATOM, 32,
                        PropModeReplace, (unsigned char *) &state, 1);
    }

    if (prop) {
        XFree(prop);
    }
}

Bool is_net_wm_state_set(Window window, Atom state) {
    Atom actual_type;
    int actual_format;
    unsigned long nitems;
    unsigned long bytes_after;
    Atom *states;
    Bool set = False;

    states = NULL;
    if (XGetWindowProperty(
            display, window, net_atoms[_NET_WM_STATE], 0L, ~0L, False, XA_ATOM,
            &actual_type, &actual_format, &nitems, &bytes_after, (unsigned char **) &states) == Success &&
        actual_type == XA_ATOM && actual_format == 32 && states) {
        while (nitems) {
            if (states[--nitems] == state) {
                set = True;
                break;
            }
        }
    }

    if (states) {
        XFree(states);
    }
    return set;
}


void send_protocol(Window window, Atom protocol) {
    Atom *protocols;
    int count;
    XEvent event;

    if (XGetWMProtocols(display, window, &protocols, &count)) {
        while (count) {
            if (protocols[--count] == protocol) {
                event.type = ClientMessage;
                event.xclient.window = window;
                event.xclient.message_type = wm_atoms[WM_PROTOCOLS];
                event.xclient.format = 32;
                event.xclient.data.l[0] = protocol;
                event.xclient.data.l[1] = CurrentTime;
                XSendEvent(display, window, False, NoEventMask, &event);
                break;
            }
        }
        XFree(protocols);
    }
}

void read_resources()
{
    Colormap map;
    char *xrm;
    XrmDatabase xrdb;
    char *type[20];
    XrmValue value;
    XColor color;

    map = DefaultColormap(display, screen);
    XrmInitialize();
    xrm = XResourceManagerString(display);
    if (xrm != NULL) {
        xrdb = XrmGetStringDatabase(xrm);
        if (XrmGetResource(xrdb, "wmd.foreground", "*", type, &value)) {
            XAllocNamedColor(display, map, value.addr, &color, &color);
        } else {
            XAllocNamedColor(display, map, "white", &color, &color);
        }
        foreground = color.pixel;
        if (XrmGetResource(xrdb, "wmd.background", "*", type, &value)) {
            XAllocNamedColor(display, map, value.addr, &color, &color);
        } else {
            XAllocNamedColor(display, map, "black", &color, &color);
        }
        background = color.pixel;
        if (XrmGetResource(xrdb, "wmd.gapSize", "*", type, &value)) {
            gap_size = atoi(value.addr);
        } else {
            gap_size = 0;
        }
        if (XrmGetResource(xrdb, "wmd.borderSize", "*", type, &value)) {
            border_size = atoi(value.addr);
        } else {
            border_size = 0;
        }
        if (XrmGetResource(xrdb, "wmd.topPadding", "*", type, &value)) {
            top_padding = atoi(value.addr);
        } else {
            top_padding = 0;
        }
        XrmDestroyDatabase(xrdb);
    }
}

Bool is_dock_window(Window window) {
    return get_atom_property(window, net_atoms[_NET_WM_WINDOW_TYPE]) == net_atoms[_NET_WM_WINDOW_TYPE_DOCK];
}

Bool is_manageable_window(Window window) {
    XWindowAttributes attributes;
    XWMHints *hints;

    hints = NULL;

    Bool manageable = False;

    /* TODO ignore if no hints */

    if (window != None &&
        window != root &&
        XGetWindowAttributes(display, window, &attributes) &&
        !attributes.override_redirect &&
        /* ((hints = XGetWMHints(display, window)) && !hints->input) || */
        !is_dock_window(window)) {
        manageable = True;
    }

    if (hints) {
        XFree(hints);
    }

    return manageable;
}

Bool is_managed_window(Window window) {
    XWindowAttributes attributes;

    return (is_manageable_window(window) &&
            XGetWindowAttributes(display, window, &attributes) &&
            (attributes.map_state == IsViewable ||
             get_wm_state(window) == IconicState));
}

Bool is_above_window(Window window) {
    return is_managed_window(window) && is_net_wm_state_set(window, net_atoms[_NET_WM_STATE_ABOVE]);
}

Bool is_not_above_window(Window window) {
    return is_managed_window(window) && !is_net_wm_state_set(window, net_atoms[_NET_WM_STATE_ABOVE]);
}

Bool is_normal_window(Window window) {
    return is_not_above_window(window) && get_wm_state(window) == NormalState;
}

unsigned int get_windows(Bool (*predicate)(Window), Window **windows) {
    Window parent;
    Window *children;
    unsigned int nchildren;
    Window window;
    unsigned int nwindows;
    int start;
    int end;

    XQueryTree(display, root, &root, &parent, &children, &nchildren);

    nwindows = 0;
    for (unsigned int i = 0; i < nchildren; i++) {
        window = children[i];
        if (predicate(window)) {
            children[nwindows++] = window;
        }
    }

    start = 0;
    end = nwindows - 1;
    while (start < end) {
        window = children[start];
        children[start++] = children[end];
        children[end--] = window;
    }

    if (nwindows == 0) {
        if (children) {
            XFree(children);
        }
    } else {
        *windows = children;
    }
    return nwindows;
}

Window get_first_window(Bool (*predicate)(Window)) {
    Window parent;
    Window *children = NULL;
    unsigned int nchildren;
    Window window = None;

    XQueryTree(display, root, &root, &parent, &children, &nchildren);

    while(nchildren) {
        window = children[--nchildren];
        if (predicate(window)) {
            break;
        } else {
            window = None;
        }
    }

    if (children) {
        XFree(children);
    }

    return window;
}

unsigned int get_managed_windows(Window **windows) {
    return get_windows(&is_managed_window, windows);
}

void print_window(FILE *stream, char *prefix, Window window, char *global_flags) {
    XWindowAttributes attributes = { 0, 0, 0, 0 };
    XClassHint class = { NULL, NULL };
    char *class_name = "";
    char *class_class = "";
    XTextProperty name;
    char *name_name = "";
    name.value = NULL;
    char flags[FLAG_COUNT];
    flags[0] = '\0';

    // TODO include pid?
    if (global_flags) {
        strcat(flags, global_flags);
    }

    if (window == root) {
        strcat(flags, FLAG_ROOT);
        attributes.width = screen_width;
        attributes.height = screen_height;
    } else if (window != None) {
        XGetWindowAttributes(display, window, &attributes);
        if (is_net_wm_state_set(window, net_atoms[_NET_WM_STATE_FULLSCREEN])) {
            strcat(flags, FLAG_FULLSCREEN);
        }
        if (is_net_wm_state_set(window, net_atoms[_NET_WM_STATE_ABOVE])) {
            strcat(flags, FLAG_ABOVE);
        }
        if (get_wm_state(window) == IconicState) {
            strcat(flags, FLAG_ICONIC);
        }

        XGetClassHint(display, window, &class);
        if (class.res_name) {
            class_name = class.res_name;
        }
        if (class.res_class) {
            class_class = class.res_class;
        }
        if ((XGetTextProperty(display, window, &name, net_atoms[_NET_WM_NAME]) ||
             XGetWMName(display, window, &name)) &&
            name.value) {
            name_name = (char *)name.value;
        }
    }
    if (stream) {
        fprintf(stream,
                "%s0x%07lx\t%s\t%d\t%d\t%d\t%d\t%s\t%s\t%s\n",
                prefix ? prefix : "",
                window,
                *flags ? flags : " ",
                attributes.width,
                attributes.height,
                attributes.x,
                attributes.y,
                class_name,
                class_class,
                name_name);
        fflush(stream);
    }
    if (class.res_name) {
        XFree(class.res_name);
    }
    if (class.res_class) {
        XFree(class.res_class);
    }
    if (name.value) {
        XFree(name.value);
    }
}

void tile_window(Window window,
                 int grid_width,
                 int grid_height,
                 int width,
                 int height,
                 int x,
                 int y) {
    int tile_width;
    int tile_height;
    XSizeHints hints;
    long supplied;
    XWindowAttributes attributes;
    Atom type;
    int border_size;
    int window_x;
    int window_y;
    int window_width;
    int window_height;

    tile_width = (screen_width - gap_size) / grid_width;
    tile_height = (screen_height - top_padding - gap_size) / grid_height;

    XGetWindowAttributes(display, window, &attributes);

    hints.flags = 0;
    XGetWMNormalHints(display, window, &hints, &supplied);

    type = get_atom_property(window, net_atoms[_NET_WM_WINDOW_TYPE]);

    border_size = get_border_size(window);

    window_x = gap_size + tile_width * x;
    window_y = top_padding + gap_size + tile_height * y;
    window_width = tile_width * width - gap_size - border_size * 2;
    window_height = tile_height * height - gap_size - border_size * 2;

    if (hints.flags & PPosition && hints.flags & PSize) {
        window_x = hints.x;
        window_y = hints.y;
        window_width = hints.width;
        window_height = hints.height;
    } else if (type == net_atoms[_NET_WM_WINDOW_TYPE_DIALOG] ||
               type == net_atoms[_NET_WM_WINDOW_TYPE_SPLASH]) {
        if (attributes.width < window_width) {
            window_x += (window_width - attributes.width) / 2;
            window_width = attributes.width;
        }
        if (attributes.height < window_height) {
            window_y += (window_height - attributes.height) / 2;
            window_height = attributes.height;
        }
    } else if (hints.flags & PMaxSize &&
               (hints.max_width < window_width ||
                hints.max_height < window_height)) {
        if (hints.max_width < window_width) {
            window_x += (window_width - hints.max_width) / 2;
            window_width = hints.max_width;
        }
        if (hints.max_height < window_height) {
            window_y += (window_height - hints.max_height) / 2;
            window_height = hints.max_height;
        }
    } else if (hints.flags & PAspect) {
    }

    if (hints.flags & PResizeInc) {
        if (hints.width_inc) {
            window_width -= window_width % hints.width_inc;
        }
        if (hints.height_inc) {
            window_height -= window_height % hints.height_inc;
        }
    }

    set_net_wm_state(window, net_atoms[_NET_WM_STATE_FULLSCREEN], False);
    XSetWindowBorderWidth(display, window, border_size);
    XMoveResizeWindow(display, window, window_x, window_y, window_width, window_height);
}

void fullscreen_window(Window window) {
    set_net_wm_state(window, net_atoms[_NET_WM_STATE_FULLSCREEN], True);
    XMoveResizeWindow(display, window, 0, 0, screen_width, screen_height);
    XSetWindowBorderWidth(display, window, 0);
    XRaiseWindow(display, window);
}

void raise_window(Window window) {
    Window *windows = NULL;
    unsigned int nwindows;

    nwindows = get_windows(&is_above_window, &windows);
    if (!nwindows) {
        XRaiseWindow(display, window);
    } else {
        XWindowChanges changes;
        changes.sibling = windows[nwindows - 1];
        changes.stack_mode = Below;
        XConfigureWindow(display, window, CWSibling|CWStackMode, &changes);
    }

    if (windows) {
        XFree(windows);
    }
}

void activate_window(Window window) {
    Window active;

    active = get_active_window();

    if (window == None || window == root) {
        if (active && is_normal_window(active)) {
            window = active;
        } else {
            window = get_first_window(&is_normal_window);
        }
    }
    if (is_managed_window(window)) {
        if (get_wm_state(window) == IconicState) {
            iconify_window(window, False);
        }
        if (active && window != active) {
            XSetWindowBorder(display, active, background);
        }
        XSetInputFocus(display, window, RevertToPointerRoot, CurrentTime);
        if (window != active) {
            XSetWindowBorder(display, window, foreground);
            raise_window(window);
            send_protocol(window, wm_atoms[WM_TAKE_FOCUS]);
            set_window_property(root, net_atoms[_NET_ACTIVE_WINDOW], window);
            print_window(fifo, prefix, window, FLAG_ACTIVE);
        }
    } else if (active) {
        set_window_property(root, net_atoms[_NET_ACTIVE_WINDOW], None);
        print_window(fifo, prefix, None, NULL);
    }
}

void iconify_window(Window window, Bool iconify) {
    if (iconify) {
        set_wm_state(window, IconicState);
        XUnmapWindow(display, window);
        Window focus;
        int revert_to;
        XGetInputFocus(display, &focus, &revert_to);
        if (window == focus) {
            XSetInputFocus(display, root, RevertToPointerRoot, CurrentTime);
        }
    } else {
        XMapWindow(display, window);
        set_wm_state(window, NormalState);
    }
}

/* quit */
/* restart */
/* windows */
/* activate <window>... */
/* tile <grid_w>x<grid_h> <w>x<h>x+<x>+<y> <window>... */
/* delete <window>... */
/* fullscreen <window>... */
/* restore <windows> */

void handle_command(char *cmd_buf, int cmd_len, FILE *response)
{
    int args_size = 4;
    // TODO could use malloc here
    char **args = calloc(args_size, sizeof(char *));
    int args_len = 0;
    int beg = 0;

    for (int i = 0; i <= cmd_len; i++) {
        if (cmd_buf[i] == '\0') {
            if (args_len == args_size) {
                args_size *= 2;
                args = realloc(args, args_size * sizeof(char *));
            }
            args[args_len++] = cmd_buf + beg;
            beg = i + 1;
        }
    }

    if (args_len == 0) {
        fprintf(response, "%c", '1');
    } else if (!strcmp(args[0], "quit")) {
        quit = True;
        fprintf(response, "%c", '0');
    } else if (!strcmp(args[0], "restart")) {
        restart = True;
        fprintf(response, "%c", '0');
    } else if (!strcmp(args[0], "windows")) {
        Window *windows = NULL;
        unsigned int nwindows;
        fprintf(response, "%c", '0');
        Window pointer = get_pointer_window();
        Window active = get_active_window();
        char flags[FLAG_COUNT];
        nwindows = get_managed_windows(&windows);
        for (unsigned int i = 0; i < nwindows; i++) {
            flags[0] = '\0';
            if (windows[i] == active) {
                strcat(flags, FLAG_ACTIVE);
            }
            if (windows[i] == pointer) {
                strcat(flags, FLAG_POINTER);
            }
            print_window(response, NULL, windows[i], flags);
        }
        if (windows) {
            XFree(windows);
        }
        print_window(response, NULL, root, NULL);
    } else if (args_len == 1) {
        fprintf(response, "%c", '1');
    } else {
        int i = 1;
        int grid_w;
        int grid_h;
        int w;
        int h;
        int x;
        int y;
        Window window;
        if (!strcmp(args[0], "tile") &&
             (args_len < 4 ||
              sscanf(args[i++], "%dx%d", &grid_w, &grid_h) < 2 ||
              sscanf(args[i++], "%dx%d+%d+%d", &w, &h, &x, &y) < 4 ||
              grid_w < 0 || grid_h < 0 ||
              w < 1 || h < 1 || w > grid_w || h > grid_h ||
              x < 0 || y < 0 || x >= grid_w || y >= grid_h)) {
            fprintf(response, "%c", '1');
            i = args_len;
        } else {
            fprintf(response, "%c", '0');
        }
        for (; i < args_len; i++) {
            if (!sscanf(args[i], "0x%lx", &window) &&
                !sscanf(args[i], "%lu", &window)) {
            } else if (!is_managed_window(window)) {
            } else if (!strcmp(args[0], "activate")) {
                activate_window(window);
            } else if (!strcmp(args[0], "delete")) {
                send_protocol(window, wm_atoms[WM_DELETE_WINDOW]);
            } else if (!strcmp(args[0], "fullscreen")){
                fullscreen_window(window);
            } else if (!strcmp(args[0], "tile")) {
                tile_window(window, grid_w, grid_h, w, h, x, y);
            } else if (!strcmp(args[0], "iconify")) {
                iconify_window(window, True);
            }
        }
    }
    free(args);
    XSync(display, False);
    fflush(response);
    fclose(response);
}

void map_window(XMapRequestEvent *request) {
    Window window = request->window;
    if (is_manageable_window(window)) {
        // TODO ResizeRedirectMask
        XSelectInput(display, window, PropertyChangeMask|FocusChangeMask|StructureNotifyMask);
        if (is_net_wm_state_set(window, net_atoms[_NET_WM_STATE_FULLSCREEN])) {
            fullscreen_window(window);
        } else {
            // TODO if specified size & position
            tile_window(window, 1, 1, 1, 1, 0, 0);
        }
        XWMHints *hints = NULL;
        hints = XGetWMHints(display, window);
        if (hints && hints->initial_state == IconicState) {
            set_wm_state(window, IconicState);
        } else {
            set_wm_state(window, NormalState);
            XMapWindow(display, window);
            activate_window(window);
        }
        if (hints) {
            XFree(hints);
        }
    } else {
        XMapWindow(display, window);
    }
}

void configure_window(XConfigureRequestEvent *request) {
    Window window = request->window;
    XWindowChanges changes;
    unsigned value_mask = request->value_mask;
    XSizeHints hints;
    long supplied;
    Atom type;

    hints.flags = 0;
    XGetWMNormalHints(display, window, &hints, &supplied);
    type = get_atom_property(window, net_atoms[_NET_WM_WINDOW_TYPE]);
    if (!is_managed_window(window) ||
        (hints.flags & PPosition && hints.flags & PSize)) {
        changes.x = request->x;
        changes.y = request->y;
        changes.width = request->width;
        changes.height = request->height;
    } else if (type == net_atoms[_NET_WM_WINDOW_TYPE_DIALOG] ||
               type == net_atoms[_NET_WM_WINDOW_TYPE_SPLASH]) {
        XWindowAttributes attributes;
        XGetWindowAttributes(display, window, &attributes);
        value_mask &= ~(CWX|CWY);
        if (value_mask & CWWidth) {
            changes.width = request->width;
            changes.x = attributes.x + (attributes.width - changes.width) / 2;
            value_mask |= CWX;
        }
        if (value_mask & CWHeight) {
            changes.height = request->height;
            changes.y = attributes.y + (attributes.height - changes.height) / 2;
            value_mask |= CWY;
        }
    } else {
        value_mask &= ~(CWX|CWY|CWWidth|CWHeight);
    }
    changes.border_width = request->border_width;
    if (value_mask & CWStackMode &&
        request->detail == Above &&
        request->above == None) {
        request->detail = Below;
        Window *windows = NULL;
        unsigned int nwindows;
        nwindows = get_windows(&is_not_above_window, &windows);
        for (unsigned int i = 0; i < nwindows; i++) {
            if (windows[i] != window) {
                request->detail = Above;
                changes.sibling = windows[i];
                value_mask |= CWSibling;
                break;
            }
        }
        if (windows) {
            XFree(windows);
        }
    } else {
        changes.sibling = request->above;
    }
    changes.stack_mode = request->detail;
    XConfigureWindow(display, window, value_mask, &changes);
}

void handle_event(XEvent *event) {
    Window window;
    switch(event->type) {
        case MapRequest:
            map_window(&event->xmaprequest);
            break;
        case ConfigureRequest:
            configure_window(&event->xconfigurerequest);
            break;
        case FocusIn:
            if ((event->xfocus.mode == NotifyNormal ||
                 event->xfocus.mode == NotifyWhileGrabbed) &&
                event->xfocus.window == root) {
                activate_window(None);
            }
            break;
        case ConfigureNotify:
            if (event->xconfigure.window == root &&
                (screen_width != event->xconfigure.width ||
                 screen_height != event->xconfigure.height)) {
                screen_width = event->xconfigure.width;
                screen_height = event->xconfigure.height;
                print_window(fifo, prefix, root, NULL);
            }
            break;
        case PropertyNotify:
            window = event->xproperty.window;
            if ((event->xproperty.atom == XA_WM_NAME ||
                 event->xproperty.atom == net_atoms[_NET_WM_NAME]) &&
                window == get_active_window()) {
                print_window(fifo, prefix, event->xproperty.window, FLAG_ACTIVE);
            }
            /* else if (event->xproperty.atom == net_atoms[_NET_WM_STATE] && */
            /*            is_managed_window(window)) { */
            /* } */
            break;
        case ClientMessage:
            window = event->xclient.window;
            if (is_managed_window(window)) {
            }
            if (event->xclient.message_type == wm_atoms[WM_CHANGE_STATE]) {
                if (event->xclient.data.l[0] == IconicState) {
                    iconify_window(window, True);
                } else if (event->xclient.data.l[0] == NormalState) {
                    iconify_window(window, False);
                }
            } else if (event->xclient.message_type == net_atoms[_NET_WM_STATE]) {
                if (event->xclient.data.l[1] == net_atoms[_NET_WM_STATE_ABOVE] ||
                    event->xclient.data.l[2] == net_atoms[_NET_WM_STATE_ABOVE]) {
                    switch (event->xclient.data.l[0]) {
                        case _NET_WM_STATE_REMOVE:
                            set_net_wm_state(window, net_atoms[_NET_WM_STATE_ABOVE], False);
                            break;
                        case _NET_WM_STATE_ADD:
                            set_net_wm_state(window, net_atoms[_NET_WM_STATE_ABOVE], True);
                            XRaiseWindow(display, window);
                            break;
                        case _NET_WM_STATE_TOGGLE:
                            break;
                    }
                }
                if (event->xclient.data.l[1] == net_atoms[_NET_WM_STATE_FULLSCREEN] ||
                    event->xclient.data.l[2] == net_atoms[_NET_WM_STATE_FULLSCREEN]) {
                    switch (event->xclient.data.l[0]) {
                        case _NET_WM_STATE_REMOVE:
                            tile_window(window, 1, 1, 1, 1, 0, 0);
                            break;
                        case _NET_WM_STATE_ADD:
                            fullscreen_window(window);
                            break;
                        case _NET_WM_STATE_TOGGLE:
                            if (is_net_wm_state_set(window, net_atoms[_NET_WM_STATE_FULLSCREEN])) {
                                tile_window(window, 1, 1, 1, 1, 0, 0);
                            } else {
                                fullscreen_window(window);
                            }
                            break;
                    }
                }
            } else if (event->xclient.message_type == net_atoms[_NET_ACTIVE_WINDOW]) {
                activate_window(event->xclient.window);
            }
            break;
    }
    XSync(display, False);
}

void handle_signal(int signal) {
    if (signal == SIGINT || signal == SIGTERM) {
        quit = True;
    }
}

int main(int argc, char *argv[]) {
    int opt;
    char *fifo_path = NULL;
    int fifo_fd;
    int x_fd;
    char *sock_dir;
    int sock_fd;
    struct sockaddr_un sock_addr;
    fd_set fds;

    while ((opt = getopt(argc, argv, "p:s:")) != -1) {
        switch (opt) {
            case 'p':
                prefix = optarg;
                break;
            case 's':
                fifo_path = optarg;
                break;
            case '?':
                fprintf(stderr, "\n");
                exit(EXIT_FAILURE);
        }
    }

    if (!(display = XOpenDisplay(NULL))) {
        fprintf(stderr, "\n");
        exit(EXIT_FAILURE);
    }

    XSetErrorHandler(error_handler);

    screen = DefaultScreen(display);
    screen_width = XDisplayWidth(display, screen);
    screen_height = XDisplayHeight(display, screen);
    root = RootWindow(display, screen);
    read_resources();
    XSelectInput(display, root, StructureNotifyMask|SubstructureNotifyMask|SubstructureRedirectMask|FocusChangeMask);

    wm_atoms[WM_PROTOCOLS] = XInternAtom(display, "WM_PROTOCOLS", False);
    wm_atoms[WM_STATE] = XInternAtom(display, "WM_STATE", False);
    wm_atoms[WM_CHANGE_STATE] = XInternAtom(display, "WM_CHANGE_STATE", False);
    wm_atoms[WM_TAKE_FOCUS] = XInternAtom(display, "WM_TAKE_FOCUS", False);
    wm_atoms[WM_DELETE_WINDOW] = XInternAtom(display, "WM_DELETE_WINDOW", False);

    net_atoms[_NET_SUPPORTED] = XInternAtom(display, "_NET_SUPPORTED", False);
    net_atoms[_NET_SUPPORTING_WM_CHECK] = XInternAtom(display, "_NET_SUPPORTING_WM_CHECK", False);
    net_atoms[_NET_ACTIVE_WINDOW] = XInternAtom(display, "_NET_ACTIVE_WINDOW", False);
    net_atoms[_NET_WM_NAME] = XInternAtom(display, "_NET_WM_NAME", False);
    net_atoms[_NET_WM_STATE] = XInternAtom(display, "_NET_WM_STATE", False);
    net_atoms[_NET_WM_STATE_ABOVE] = XInternAtom(display, "_NET_WM_STATE_ABOVE", False);
    net_atoms[_NET_WM_STATE_FULLSCREEN] = XInternAtom(display, "_NET_WM_STATE_FULLSCREEN", False);
    net_atoms[_NET_WM_WINDOW_TYPE] = XInternAtom(display, "_NET_WM_WINDOW_TYPE", False);
    net_atoms[_NET_WM_WINDOW_TYPE_DIALOG] = XInternAtom(display, "_NET_WM_WINDOW_TYPE_DIALOG", False);
    net_atoms[_NET_WM_WINDOW_TYPE_DOCK] = XInternAtom(display, "_NET_WM_WINDOW_TYPE_DOCK", False);
    net_atoms[_NET_WM_WINDOW_TYPE_SPLASH] = XInternAtom(display, "_NET_WM_WINDOW_TYPE_SPLASH", False);
    _MOTIF_WM_HINTS = XInternAtom(display, "_MOTIF_WM_HINTS", False);

    XChangeProperty(display, root, net_atoms[_NET_SUPPORTED], XA_ATOM, 32,
                    PropModeReplace, (unsigned char *) net_atoms, net_atoms_count);

    Window *windows = NULL;
    unsigned int nwindows;
    nwindows = get_managed_windows(&windows);
    for (unsigned int i = 0; i < nwindows; i++) {
        XSelectInput(display, windows[i], PropertyChangeMask|FocusChangeMask|StructureNotifyMask);
    }
    if (windows) {
        XFree(windows);
    }

    x_fd = XConnectionNumber(display);

    if (!(sock_dir = getenv("XDG_RUNTIME_DIR"))) {
        sock_dir = "/tmp";
    }

    sock_addr.sun_family = AF_UNIX;
    snprintf(sock_addr.sun_path, sizeof(sock_addr.sun_path), "%s/wmd%s", sock_dir, XDisplayString(display) + 1);
    sock_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (sock_fd == -1) {
        fprintf(stderr, "\n");
        exit(EXIT_FAILURE);
    }

    unlink(sock_addr.sun_path);

    if (bind(sock_fd, (struct sockaddr *) &sock_addr, sizeof(sock_addr)) == -1) {
        fprintf(stderr, "\n");
        exit(EXIT_FAILURE);
    }

    if (listen(sock_fd, SOMAXCONN) == -1) {
        fprintf(stderr, "\n");
        exit(EXIT_FAILURE);
    }

    if (fifo_path != NULL) {
        fifo_fd = open(fifo_path, O_RDWR | O_NONBLOCK);
        if (fifo_fd != -1) {
            fifo = fdopen(fifo_fd, "w");
        }
        else {
            fprintf(stderr, "err\n");
        }
    }

    XEvent event;
    int cmd_fd;
    int cmd_size = 1024;
    char *cmd_buf = malloc(cmd_size);
    int cmd_len;

    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);

    Window wm_window = XCreateSimpleWindow(display, root, 0, 0, 1, 1, 0, 0, 0);
    set_window_property(wm_window, net_atoms[_NET_SUPPORTING_WM_CHECK], wm_window);
    set_window_property(root, net_atoms[_NET_SUPPORTING_WM_CHECK], wm_window);
    XChangeProperty(display, wm_window, net_atoms[_NET_WM_NAME],
                    XInternAtom(display, "UTF8_STRING", False), 8,
                    PropModeReplace, (unsigned char *) "wmd", 3);

    print_window(fifo, prefix, root, NULL);

    while(!restart && !quit) {
        FD_ZERO(&fds);
        FD_SET(sock_fd, &fds);
        FD_SET(x_fd, &fds);
        if(select(FD_SETSIZE, &fds, NULL, NULL, NULL) > 0) {
            if (FD_ISSET(x_fd, &fds)) {
                while(XPending(display) && !XNextEvent(display, &event)) {
                    handle_event(&event);
                }
            }

            if (FD_ISSET(sock_fd, &fds)) {
                cmd_fd = accept(sock_fd, NULL, 0);
                if (cmd_fd > 0) {
                    cmd_len = 0;

                    while ((cmd_len += recv(cmd_fd, cmd_buf + cmd_len, cmd_size - cmd_len, 0)) == cmd_size) {
                        cmd_size *= 2;
                        cmd_buf = realloc(cmd_buf, cmd_size);
                    }

                    if (cmd_len > 0) {
                        FILE* res = fdopen(cmd_fd, "w");
                        if (res != NULL) {
                            handle_command(cmd_buf, cmd_len, res);
                        } else {
                            close(cmd_fd);
                        }
                    }
                }
            }
        }
    }
    XDestroyWindow(display, wm_window);

    if (fifo != NULL) {
        fclose(fifo);
    }

    free(cmd_buf);

    close(sock_fd);
    unlink(sock_addr.sun_path);

    XCloseDisplay(display);

    if (restart) {
        execvp(argv[0], argv);
    }

    return EXIT_SUCCESS;
}
