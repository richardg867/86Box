/*
 * 86Box    A hypervisor and IBM PC system emulator that specializes in
 *          running old operating systems and software designed for IBM
 *          PC systems and compatibles from 1981 through fairly recent
 *          system designs based on the PCI bus.
 *
 *          This file is part of the 86Box distribution.
 *
 *          X11 Xinput2 mouse input module.
 *
 * Authors: Cacodemon345
 *          RichardG <richardg867@gmail.com>
 *
 *          Copyright 2022 Cacodemon345.
 *          Copyright 2023 RichardG.
 */
#include <QDebug>
#include <QThread>
#include <QProcess>
#include <QApplication>
#include <QAbstractNativeEventFilter>
#include <QCursor>
#include <QWidget>
#include <QWindow>

#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <unordered_map>

extern "C" {
#include <X11/Xlib.h>
#include <X11/keysym.h>
#include <X11/extensions/XInput2.h>
#include <unistd.h>
#include <fcntl.h>

#include <86box/86box.h>
#include <86box/mouse.h>
#include <86box/plat.h>
}

static Display            *disp         = nullptr;
static QThread            *procThread   = nullptr;
static QWidget            *grab_widget  = nullptr;
static std::atomic<Window> grab_window  = 0;
static std::atomic<bool>   reset_motion = true;
static int                 xi2opcode    = 0;

/* How a pointing device reports motion, which is not always what it declares. */
enum class PointerKind {
    Unknown,  /* not enough events seen yet */
    Relative, /* raw values are motion deltas */
    Absolute, /* absolute axes in device units; the position is read from the server */
    Pixels    /* relative axes, but the raw values are screen positions: XTEST pointers
                 driven by VNC servers, and the pointer of XWin (VcXsrv, Cygwin/X) */
};
struct PointerInfo {
    PointerKind kind       = PointerKind::Unknown;
    int         matches    = 0;
    int         mismatches = 0;
};
static std::unordered_map<int, PointerInfo> pointers;

/* Pointer position as last reported by an absolute or pixel device. */
static struct {
    int    source = -1; /* device the position came from, or -1 for none */
    double x      = 0.0;
    double y      = 0.0;

    /* Warping only moves the pointer of some of these devices: XWin moves the
       Windows cursor, but a tablet or a VNC client's pointer stays where it is. */
    bool   no_warp      = false;
    bool   warp_pending = false; /* warped, outcome not seen yet */
    bool   warp_echoed  = false; /* the warp came back as an event from the device */
    int    warp_events  = 0;
    double warp_x       = 0.0;
    double warp_y       = 0.0;
} pos;

/* Server pointer position from the last query, to tell pixel devices apart. */
static bool   have_query = false;
static double query_x    = 0.0;
static double query_y    = 0.0;

/* Centre of the grab window in root coordinates, refreshed periodically. */
static bool                                  have_center = false;
static int                                   center_x = 0, center_y = 0, center_radius = 0;
static std::chrono::steady_clock::time_point center_time;

/* Based on SDL2. Returns a mask of the axes present among the first output_values_len. */
static int
parse_valuators(const double        *input_values,
                const unsigned char *mask, int mask_len,
                double *output_values, int output_values_len)
{
    int i       = 0;
    int z       = 0;
    int top     = mask_len * 8;
    int present = 0;
    if (top > 16)
        top = 16;

    memset(output_values, 0, output_values_len * sizeof(output_values[0]));
    for (; i < top && z < output_values_len; i++) {
        if (XIMaskIsSet(mask, i)) {
            output_values[z] = *input_values;
            input_values++;
            present |= 1 << z;
        }
        z++;
    }
    return present;
}

static bool exitthread = false;

static PointerInfo &
xinput2_pointer_info(int deviceid)
{
    auto it = pointers.find(deviceid);
    if (it != pointers.end())
        return it->second;

    PointerInfo   info;
    int           num;
    XIDeviceInfo *dev = XIQueryDevice(disp, deviceid, &num);
    if (dev) {
        for (int i = 0; i < dev->num_classes; i++) {
            const XIValuatorClassInfo *v = (const XIValuatorClassInfo *) dev->classes[i];
            if ((v->type == XIValuatorClass) && (v->number <= 1) && (v->mode == XIModeAbsolute))
                info.kind = PointerKind::Absolute;
        }
        XIFreeDeviceInfo(dev);
    }
    return pointers.emplace(deviceid, info).first->second;
}

static bool
xinput2_query_pointer(int master, double *x, double *y)
{
    Window          root;
    Window          child;
    double          win_x;
    double          win_y;
    XIButtonState   buttons = { };
    XIModifierState mods;
    XIGroupState    group;

    Bool ret = XIQueryPointer(disp, master, DefaultRootWindow(disp), &root, &child, x, y, &win_x, &win_y, &buttons, &mods, &group);
    if (buttons.mask)
        XFree(buttons.mask);
    return ret;
}

static bool
xinput2_update_center()
{
    auto now = std::chrono::steady_clock::now();
    if (have_center && ((now - center_time) < std::chrono::milliseconds(250)))
        return true;

    Window            win = grab_window;
    XWindowAttributes attrib { };
    Window            child;
    have_center = win && XGetWindowAttributes(disp, win, &attrib) && XTranslateCoordinates(disp, win, attrib.root, attrib.width / 2, attrib.height / 2, &center_x, &center_y, &child);
    if (have_center) {
        center_radius = std::max(std::min(attrib.width, attrib.height) / 4, 16);
        center_time   = now;
    }
    return have_center;
}

static void
xinput2_warp()
{
    XWarpPointer(disp, None, DefaultRootWindow(disp), 0, 0, 0, 0, center_x, center_y);
    XFlush(disp);
}

/* Start over after the mouse is captured. */
static void
xinput2_reset()
{
    pos         = { };
    have_query  = false;
    have_center = false;
}

/* Motion of a device that reports where the pointer is, rather than how far it moved. */
static void
xinput2_position(int source, double x, double y, bool can_warp)
{
    if (pos.source != source) {
        /* First position from this device: just start from it. */
        pos        = { };
        pos.source = source;
        pos.x      = x;
        pos.y      = y;
        return;
    }

    double dx = x - pos.x;
    double dy = y - pos.y;
    if (pos.warp_pending) {
        if ((x == pos.warp_x) && (y == pos.warp_y)) {
            /* The warp coming back from the device (XWin moving the Windows cursor). */
            pos.warp_echoed = true;
            return;
        }

        if (std::hypot(x - pos.warp_x, y - pos.warp_y) < std::hypot(dx, dy)) {
            /* The warp moved the device's pointer; carry on from there. */
            dx               = x - pos.warp_x;
            dy               = y - pos.warp_y;
            pos.warp_pending = false;
        } else if (pos.warp_echoed || (++pos.warp_events >= 4)) {
            /* The device kept reporting from where it was: stop warping it. */
            pos.warp_pending = false;
            pos.no_warp      = true;
        }
        /* Otherwise, this was sent before the warp took effect. */
    }

    mouse_scalef(dx, dy);
    pos.x = x;
    pos.y = y;

    /* Keep the pointer within the window, if warping it works and is safe:
       on Xwayland, a warp locks the Wayland pointer, after which an absolute
       device stops sending events altogether. */
    if (can_warp && !pos.no_warp && !pos.warp_pending && xinput2_update_center() && ((std::fabs(x - center_x) > center_radius) || (std::fabs(y - center_y) > center_radius))) {
        pos.warp_pending = true;
        pos.warp_echoed  = false;
        pos.warp_events  = 0;
        pos.warp_x       = center_x;
        pos.warp_y       = center_y;
        xinput2_warp();
    }
}

/* Whether raw values are a pointer position: the server's pointer went through
   them on its way from (from_x, from_y) to where it is now. */
static bool
xinput2_is_position(int master, const double *coords, bool have_from, double from_x, double from_y)
{
    double x;
    double y;
    if (!xinput2_query_pointer(master, &x, &y))
        return false;

    if (!have_from) {
        from_x = x;
        from_y = y;
    }
    query_x    = x;
    query_y    = y;
    have_query = true;

    return (coords[0] >= (std::min(x, from_x) - 2.0)) && (coords[0] <= (std::max(x, from_x) + 2.0)) && (coords[1] >= (std::min(y, from_y) - 2.0)) && (coords[1] <= (std::max(y, from_y) + 2.0));
}

static void
xinput2_raw_motion(const XIRawEvent *rawev)
{
    double coords[2];
    int    present = parse_valuators(rawev->raw_values, rawev->valuators.mask, rawev->valuators.mask_len, coords, 2);
    if (!present)
        return;

    PointerInfo &info = xinput2_pointer_info(rawev->sourceid);
    double       x;
    double       y;
    switch (info.kind) {
        case PointerKind::Absolute:
            /* Read the position from the server instead of trusting the axis range:
               Xwayland's pointer declares 0-65535, but reports screen pixels. */
            if (xinput2_query_pointer(rawev->deviceid, &x, &y))
                xinput2_position(rawev->sourceid, x, y, false);
            return;

        case PointerKind::Pixels:
            /* The same XTEST device can be driven with positions by a VNC server
               and with relative motion by something else, such as Steam Input.
               Check jumps, and anything that cannot be a position, with the server. */
            x = (present & 1) ? coords[0] : pos.x;
            y = (present & 2) ? coords[1] : pos.y;
            if ((pos.source == rawev->sourceid) && (x >= 0.0) && (y >= 0.0) && (((std::fabs(x - pos.x) <= 64.0) && (std::fabs(y - pos.y) <= 64.0)) || (pos.warp_pending && (std::fabs(x - pos.warp_x) <= 64.0) && (std::fabs(y - pos.warp_y) <= 64.0)))) {
                xinput2_position(rawev->sourceid, x, y, true);
                return;
            }
            if ((present == 3) && (x >= 0.0) && (y >= 0.0) && xinput2_is_position(rawev->deviceid, coords, pos.source == rawev->sourceid, pos.x, pos.y)) {
                xinput2_position(rawev->sourceid, x, y, true);
                return;
            }
            info.kind       = PointerKind::Relative;
            info.matches    = 0;
            info.mismatches = 0;
            break;

        case PointerKind::Relative:
            /* See above. Steps this large along both axes are rare enough to check. */
            if ((present == 3) && (coords[0] >= 0.0) && (coords[1] >= 0.0) && ((coords[0] > 64.0) || (coords[1] > 64.0)) && xinput2_is_position(rawev->deviceid, coords, false, 0.0, 0.0)) {
                info.kind       = PointerKind::Unknown;
                info.matches    = 1;
                info.mismatches = 0;
                xinput2_position(rawev->sourceid, coords[0], coords[1], false);
                return;
            }
            break;

        case PointerKind::Unknown:
            /* Raw values the server's pointer went through since the last check are
               positions. Anything else, including motion along one axis only, is
               relative motion. */
            if ((present == 3) && xinput2_is_position(rawev->deviceid, coords, have_query, query_x, query_y)) {
                info.mismatches = 0;
                if (++info.matches >= 2)
                    info.kind = PointerKind::Pixels;
                xinput2_position(rawev->sourceid, coords[0], coords[1], false);
                return;
            }
            info.matches = 0;
            if (++info.mismatches >= 4)
                info.kind = PointerKind::Relative;
            break;
    }

    mouse_scalef(coords[0], coords[1]);

    /* Keep the pointer in the window, so that it does not run into the edge of
       the screen or another window. Any absolute device starts over from where
       it is when it next moves. */
    pos.source = -1;
    if (xinput2_update_center())
        xinput2_warp();
}

void
xinput2_proc()
{
    Window win;
    win = DefaultRootWindow(disp);

    XIEventMask   masks[2];
    unsigned char master_mask[XIMaskLen(XI_LASTEVENT)] = { };
    unsigned char all_mask[XIMaskLen(XI_LASTEVENT)]    = { };

    masks[0].deviceid = XIAllMasterDevices;
    masks[0].mask_len = sizeof(master_mask);
    masks[0].mask     = master_mask;
    XISetMask(master_mask, XI_RawMotion);
    XISetMask(master_mask, XI_Motion);

    masks[1].deviceid = XIAllDevices;
    masks[1].mask_len = sizeof(all_mask);
    masks[1].mask     = all_mask;
    XISetMask(all_mask, XI_HierarchyChanged);
    XISetMask(all_mask, XI_DeviceChanged);

    XISelectEvents(disp, win, masks, 2);

    XSync(disp, False);

    bool   raw_seen       = false;
    int    prev_src       = -1;
    Time   prev_time      = 0;
    double prev_coords[2] = { 0.0, 0.0 };
    while (true) {
        XEvent               ev;
        XGenericEventCookie *cookie = (XGenericEventCookie *) &ev.xcookie;
        XNextEvent(disp, (XEvent *) &ev);

        if (XGetEventData(disp, cookie) && (cookie->type == GenericEvent) && (cookie->extension == xi2opcode)) {
            switch (cookie->evtype) {
                case XI_RawMotion:
                    {
                        raw_seen = true;
                        if (!mouse_capture)
                            break;
                        const XIRawEvent *rawev = (const XIRawEvent *) cookie->data;
                        if (reset_motion.exchange(false))
                            xinput2_reset();

                        /* Ignore duplicated events. */
                        double coords[2];
                        parse_valuators(rawev->raw_values, rawev->valuators.mask, rawev->valuators.mask_len, coords, 2);
                        if ((rawev->sourceid == prev_src) && (rawev->time == prev_time) && (coords[0] == prev_coords[0]) && (coords[1] == prev_coords[1]))
                            break;
                        prev_src       = rawev->sourceid;
                        prev_time      = rawev->time;
                        prev_coords[0] = coords[0];
                        prev_coords[1] = coords[1];

                        xinput2_raw_motion(rawev);
                        break;
                    }

                case XI_Motion:
                    {
                        /* Only reaches the root window if no window selected it, and only
                           needed on servers without raw events (old VNC servers). */
                        if (raw_seen || !mouse_capture)
                            break;
                        const XIDeviceEvent *devev = (const XIDeviceEvent *) cookie->data;
                        if (reset_motion.exchange(false))
                            xinput2_reset();

                        xinput2_position(devev->sourceid, devev->root_x, devev->root_y, true);
                        break;
                    }

                case XI_HierarchyChanged:
                    pointers.clear();
                    break;

                case XI_DeviceChanged:
                    {
                        const XIDeviceChangedEvent *changeev = (const XIDeviceChangedEvent *) cookie->data;
                        if (changeev->reason == XIDeviceChange)
                            pointers.erase(changeev->sourceid);
                        break;
                    }
            }
        }

        XFreeEventData(disp, cookie);
        if (exitthread)
            break;
    }
    XCloseDisplay(disp);
}

void
xinput2_exit()
{
    if (grab_widget)
        grab_widget->releaseMouse();
    if (!exitthread) {
        exitthread = true;
        if (procThread) {
            procThread->wait(5000);
            procThread->terminate();
        }
    }
}

void
xinput2_set_grab_widget(QWidget *widget)
{
    grab_widget = widget;
}

void
xinput2_mouse_capture(QWindow *window)
{
    Q_UNUSED(window);
    reset_motion = true;
    if (grab_widget) {
        grab_window = grab_widget->winId();
        grab_widget->grabMouse(QCursor(Qt::BlankCursor));
    }
}

void
xinput2_mouse_uncapture()
{
    if (grab_widget)
        grab_widget->releaseMouse();
}

void
xinput2_init()
{
    disp = XOpenDisplay(nullptr);
    if (!disp) {
        qWarning() << "Cannot open current X11 display";
        return;
    }
    auto event = 0;
    auto err   = 0;
    auto minor = 1;
    auto major = 2;
    if (XQueryExtension(disp, "XInputExtension", &xi2opcode, &event, &err)) {
        if (XIQueryVersion(disp, &major, &minor) == Success) {
            procThread = QThread::create(xinput2_proc);
            procThread->start();
            atexit(xinput2_exit);
        }
    }
}
