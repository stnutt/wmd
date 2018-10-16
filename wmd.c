#include <fcntl.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xresource.h>
#include <X11/Xutil.h>

/* https://tronche.com/gui/x/xlib/ */
/* https://specifications.freedesktop.org/wm-spec/wm-spec-latest.html */

#define FLAG_ROOT       "r"
#define FLAG_FULLSCREEN "f"
#define FLAG_ABOVE      "t"
#define FLAG_POINTER    "p"
#define FLAG_ACTIVE     "a"
#define FLAG_URGENT     "u"
/* #define FLAG_ATTENTION */

enum {
    WM_PROTOCOLS,
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

enum {
    _MOTIF_WM_HINTS,
    motif_atoms_count
};

enum {
    _NET_WM_STATE_REMOVE,
    _NET_WM_STATE_ADD,
    _NET_WM_STATE_TOGGLE
};

static bool quit = false;
static bool restart = false;

/* X */
static Display *display;
static int screen;
static Window root;
static int screen_width;
static int screen_height;
static Atom wm_atoms[wm_atoms_count];
static Atom net_atoms[net_atoms_count];
static Atom motif_atoms[motif_atoms_count];

/* */
static FILE *fifo;

/* settings */
static unsigned int foreground;
static unsigned int background;
static int border_size;
static int gap_size;
static int top_padding;

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
    XGetWindowProperty(display, window, property, 0L, length, false, type,
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

void set_window_state(Window window, Atom state, bool set) {
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

bool is_window_state_set(Window window, Atom state) {
    Atom actual_type;
    int actual_format;
    unsigned long nitems;
    unsigned long bytes_after;
    Atom *states;
    bool set = false;

    states = NULL;
    if (XGetWindowProperty(
            display, window, net_atoms[_NET_WM_STATE], 0L, ~0L, false, XA_ATOM,
            &actual_type, &actual_format, &nitems, &bytes_after, (unsigned char **) &states) == Success &&
        actual_type == XA_ATOM && actual_format == 32 && states) {
        while (nitems) {
            if (states[--nitems] == state) {
                set = true;
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

bool is_dock_window(Window window) {
    return get_atom_property(window, net_atoms[_NET_WM_WINDOW_TYPE]) == net_atoms[_NET_WM_WINDOW_TYPE_DOCK];
}

bool is_managed_window(Window window) {
    XWindowAttributes attributes;
    XWMHints *hints;

    hints = NULL;

    bool ret = true;

    /* TODO ignore if no hints */

    if (window == None ||
        window == root ||
        !XGetWindowAttributes(display, window, &attributes) ||
        attributes.override_redirect ||
        attributes.map_state != IsViewable ||
        /* ((hints = XGetWMHints(display, window)) && !hints->input) || */
        is_dock_window(window)) {
        ret = false;
    }

    if (hints) {
        XFree(hints);
    }

    return ret;
}

bool is_above_window(Window window) {
    return is_managed_window(window) && is_window_state_set(window, net_atoms[_NET_WM_STATE_ABOVE]);
}

bool is_not_above_window(Window window) {
    return is_managed_window(window) && !is_window_state_set(window, net_atoms[_NET_WM_STATE_ABOVE]);
}

unsigned int get_windows(bool (*predicate)(Window), Window **windows) {
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

unsigned int get_managed_windows(Window **windows) {
    return get_windows(&is_managed_window, windows);
}

void print_window(FILE *stream, Window window, char *global_flags) {
    XWindowAttributes attributes;
    XClassHint class = { NULL, NULL };
    char *class_name = "";
    char *class_class = "";
    XTextProperty name;
    char *name_name = "";
    name.value = NULL;
    char flags[3] = "";

    if (is_window_state_set(window, net_atoms[_NET_WM_STATE_FULLSCREEN])) {
        strcat(flags, FLAG_FULLSCREEN);
    }
    if (is_window_state_set(window, net_atoms[_NET_WM_STATE_ABOVE])) {
        strcat(flags, FLAG_ABOVE);
    }

    XGetWindowAttributes(display, window, &attributes);
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
    fprintf(stream,
            "0x%08lx\t%s\t%s\t%d\t%d\t%d\t%d\t%s\t%s\t%s\n",
            window,
            global_flags,
            flags,
            attributes.width,
            attributes.height,
            attributes.x,
            attributes.y,
            class_name,
            class_class,
            name_name);
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
    XSizeHints hints;
    long supplied;
    int tile_width;
    int tile_height;
    int window_width;
    int window_height;
    int window_x;
    int window_y;
    hints.flags = 0;

    // If there is a specified size and position, use it and do not do any tiling
    // If there is a specified position, ignore it
    // If there is a specified size, tile but keep size (or limit to specified size?)
    // obey aspect ratio
    // obey max size
    // obey increments
    // TODO center certain windows
    if (XGetWMNormalHints(display, window, &hints, &supplied)) {
        /* program specified size */
        /* if (hints.flags & PSize) { */
        /*     return; */
        /* } */
        if (hints.flags & PMaxSize) {
        }
        if (hints.flags & PResizeInc) {
        }
    }

    /* XGetWindowProperty */
    if (hints.flags & PSize && hints.flags & PPosition) {
        window_width = hints.width;
        window_height = hints.height;
        window_x = hints.x;
        window_y = hints.y;
    } else {

        tile_width = (screen_width - gap_size) / grid_width;
        tile_height = (screen_height - top_padding - gap_size) / grid_height;

        window_width = tile_width * width - gap_size;
        window_height = tile_height * height - gap_size;

        window_x = gap_size + tile_width * x;
        window_y = top_padding + gap_size + tile_height * y;
    }
    // TODO or dont factor in border size for program psotion + size???

    window_width -= border_size * 2;
    window_height -= border_size * 2;

    window_x += border_size;
    window_y += border_size;

    set_window_state(window, net_atoms[_NET_WM_STATE_FULLSCREEN], false);
    XSetWindowBorderWidth(display, window, border_size);
    XMoveResizeWindow(display, window, window_x, window_y, window_width, window_height);
}

void fullscreen_window(Window window) {
    set_window_state(window, net_atoms[_NET_WM_STATE_FULLSCREEN], true);
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
    Window *windows;

    active = get_active_window();

    if (window == None) {
        if (active && is_managed_window(active)) {
            window = active;
        } else if (get_windows(&is_not_above_window, &windows)) {
            window = windows[0];
            XFree(windows);
        }
    }
    if (is_managed_window(window)) {
        if (active && window != active) {
            XSetWindowBorder(display, active, background);
        }
        XSetInputFocus(display, window, RevertToPointerRoot, CurrentTime);
        if (window != active) {
            XSetWindowBorder(display, window, foreground);
            raise_window(window);
            send_protocol(window, wm_atoms[WM_TAKE_FOCUS]);
            set_window_property(root, net_atoms[_NET_ACTIVE_WINDOW], window);
            fputc('W', fifo);
            print_window(fifo, window, FLAG_ACTIVE);
        }
    } else if (active) {
        set_window_property(root, net_atoms[_NET_ACTIVE_WINDOW], None);
        fprintf(fifo, "W\n");
    }
    fflush(fifo);
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
        quit = true;
        fprintf(response, "%c", '0');
    } else if (!strcmp(args[0], "restart")) {
        restart = true;
        fprintf(response, "%c", '0');
    } else if (!strcmp(args[0], "root")) {
        fprintf(response, "%c", '0');
        print_window(response, root, FLAG_ROOT);
    } else if (!strcmp(args[0], "windows")) {
        Window *windows = NULL;
        unsigned int nwindows;
        fprintf(response, "%c", '0');
        Window pointer = get_pointer_window();
        Window active = get_active_window();
        char flags[3];
        nwindows = get_managed_windows(&windows);
        for (unsigned int i = 0; i < nwindows; i++) {
            flags[0] = '\0';
            if (windows[i] == active) {
                strcat(flags, FLAG_ACTIVE);
            }
            if (windows[i] == pointer) {
                strcat(flags, FLAG_POINTER);
            }
            print_window(response, windows[i], flags);
        }
        if (windows) {
            XFree(windows);
        }
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
            if (!sscanf(args[i], "0x%lx", &window)) {
            } else if (!is_managed_window(window)) {
            } else if (!strcmp(args[0], "activate")) {
                activate_window(window);
            } else if (!strcmp(args[0], "delete")) {
                send_protocol(window, wm_atoms[WM_DELETE_WINDOW]);
            } else if (!strcmp(args[0], "fullscreen")){
                fullscreen_window(window);
            } else if (!strcmp(args[0], "tile")) {
                tile_window(window, grid_w, grid_h, w, h, x, y);
            }
        }
    }
    XFlush(display);
    fflush(fifo);
    fflush(response);
    fclose(response);
}

Window map_window(XMapRequestEvent *request)
{
    Window window = request->window;
    XMapWindow(display, window);
    if (is_managed_window(window)) {
        // TODO ResizeRedirectMask
        XSelectInput(display, window, PropertyChangeMask|FocusChangeMask);
        if (is_window_state_set(window, net_atoms[_NET_WM_STATE_FULLSCREEN])) {
            fullscreen_window(window);
        } else {
            // TODO if specified size & position
            tile_window(window, 1, 1, 1, 1, 0, 0);
        }
        activate_window(window);
    } else {
        window = None;
    }

    return window;
}

void configure_window(XConfigureRequestEvent *request)
{
    XWindowChanges changes;
    changes.x = request->x;
    changes.y = request->y;
    changes.width = request->width;
    changes.height = request->height;
    changes.border_width = request->border_width;
    changes.sibling = request->above;
    changes.stack_mode = request->detail;
    XConfigureWindow(display, request->window, request->value_mask, &changes);
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
            if (event->xfocus.mode == NotifyNormal) {
                window = event->xfocus.window;
                if (window == root) {
                    activate_window(None);
                } else if (is_managed_window(window) &&
                           (event->xfocus.detail == NotifyNonlinear ||
                            event->xfocus.detail == NotifyNonlinearVirtual)) {
                    activate_window(window);
                }
            }
            break;
        case ConfigureNotify:
            if (event->xconfigure.window == root &&
                (screen_width != event->xconfigure.width ||
                 screen_height != event->xconfigure.height)) {
                screen_width = event->xconfigure.width;
                screen_height = event->xconfigure.height;
                fputc('W', fifo);
                print_window(fifo, root, FLAG_ROOT);
            }
            break;
        case PropertyNotify:
            window = event->xproperty.window;
            if ((event->xproperty.atom == XA_WM_NAME ||
                 event->xproperty.atom == net_atoms[_NET_WM_NAME]) &&
                window == get_active_window()) {
                fputc('W', fifo);
                print_window(fifo, event->xproperty.window, FLAG_ACTIVE);
            }
            /* else if (event->xproperty.atom == net_atoms[_NET_WM_STATE] && */
            /*            is_managed_window(window)) { */
            /* } */
            break;
        case ClientMessage:
            window = event->xclient.window;
            if (is_managed_window(window)) {
            }
            if (event->xclient.message_type == net_atoms[_NET_WM_STATE]) {
                if (event->xclient.data.l[1] == net_atoms[_NET_WM_STATE_ABOVE] ||
                    event->xclient.data.l[2] == net_atoms[_NET_WM_STATE_ABOVE]) {
                    switch (event->xclient.data.l[0]) {
                        case _NET_WM_STATE_REMOVE:
                            set_window_state(window, net_atoms[_NET_WM_STATE_ABOVE], false);
                            break;
                        case _NET_WM_STATE_ADD:
                            set_window_state(window, net_atoms[_NET_WM_STATE_ABOVE], true);
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
                            if (is_window_state_set(window, net_atoms[_NET_WM_STATE_FULLSCREEN])) {
                                tile_window(window, 1, 1, 1, 1, 0, 0);
                            } else {
                                fullscreen_window(window);
                            }
                            break;
                    }
                }
            } else if (event->xclient.message_type == net_atoms[_NET_ACTIVE_WINDOW]) {
                // TODO
            }
            break;
    }
    XFlush(display);
    fflush(fifo);
}

void handle_signal(int signal) {
    if (signal == SIGINT || signal == SIGTERM) {
        quit = true;
    }
}

int main(int argc, char *argv[]) {
    int opt;
    char *fifo_path;
    struct stat fifo_stat;
    int fifo_fd;
    int x_fd;
    char *sock_dir;
    int sock_fd;
    struct sockaddr_un sock_addr;
    fd_set fds;

    fifo_path = getenv("STATUS_FIFO");
    while ((opt = getopt(argc, argv, "s:")) != -1) {
        switch (opt) {
            case 's':
                fifo_path = optarg;
                break;
        }
    }

    if (!fifo_path ||
        stat(fifo_path, &fifo_stat) ||
        !S_ISFIFO(fifo_stat.st_mode)) {
        fprintf(stderr, "\n");
        fifo_path = "/dev/null";
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

    wm_atoms[WM_PROTOCOLS] = XInternAtom(display, "WM_PROTOCOLS", false);
    wm_atoms[WM_TAKE_FOCUS] = XInternAtom(display, "WM_TAKE_FOCUS", false);
    wm_atoms[WM_DELETE_WINDOW] = XInternAtom(display, "WM_DELETE_WINDOW", false);

    net_atoms[_NET_SUPPORTED] = XInternAtom(display, "_NET_SUPPORTED", false);
    net_atoms[_NET_SUPPORTING_WM_CHECK] = XInternAtom(display, "_NET_SUPPORTING_WM_CHECK", false);
    net_atoms[_NET_ACTIVE_WINDOW] = XInternAtom(display, "_NET_ACTIVE_WINDOW", false);
    net_atoms[_NET_WM_NAME] = XInternAtom(display, "_NET_WM_NAME", false);
    net_atoms[_NET_WM_STATE] = XInternAtom(display, "_NET_WM_STATE", false);
    net_atoms[_NET_WM_STATE_ABOVE] = XInternAtom(display, "_NET_WM_STATE_ABOVE", false);
    net_atoms[_NET_WM_STATE_FULLSCREEN] = XInternAtom(display, "_NET_WM_STATE_FULLSCREEN", false);
    net_atoms[_NET_WM_WINDOW_TYPE] = XInternAtom(display, "_NET_WM_WINDOW_TYPE", false);
    net_atoms[_NET_WM_WINDOW_TYPE_DIALOG] = XInternAtom(display, "_NET_WM_WINDOW_TYPE_DIALOG", false);
    net_atoms[_NET_WM_WINDOW_TYPE_DOCK] = XInternAtom(display, "_NET_WM_WINDOW_TYPE_DOCK", false);
    net_atoms[_NET_WM_WINDOW_TYPE_SPLASH] = XInternAtom(display, "_NET_WM_WINDOW_TYPE_SPLASH", false);
    motif_atoms[_MOTIF_WM_HINTS] = XInternAtom(display, "_MOTIF_WM_HINTS", false);

    XChangeProperty(display, root, net_atoms[_NET_SUPPORTED], XA_ATOM, 32,
                    PropModeReplace, (unsigned char *) net_atoms, net_atoms_count);

    Window *windows = NULL;
    unsigned int nwindows;
    nwindows = get_managed_windows(&windows);
    for (unsigned int i = 0; i < nwindows; i++) {
        XSelectInput(display, windows[i], PropertyChangeMask|FocusChangeMask);
    }
    if (windows) {
        XFree(windows);
    }

    x_fd = XConnectionNumber(display);

    if (!(sock_dir = getenv("XDG_RUNTIME_DIR"))) {
        sock_dir = "/tmp";
    }

    sock_addr.sun_family = AF_UNIX;
    snprintf(sock_addr.sun_path, sizeof(sock_addr.sun_path), "%s/wmd%s", sock_dir, XDisplayString(display));
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
                    XInternAtom(display, "UTF8_STRING", false), 8,
                    PropModeReplace, (unsigned char *) "wmd", 3);

    fputc('W', fifo);
    print_window(fifo, root, FLAG_ROOT);

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
