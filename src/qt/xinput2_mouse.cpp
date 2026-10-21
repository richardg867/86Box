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
    double      last_x     = 0.0; /* last position, while still Unknown */
    double      last_y     = 0.0;
};
static std::unordered_map<int, PointerInfo> pointers;

/* Pointer position as last reported by an absolute or pixel device. */
static struct {
    int    source = -1; /* device the position came from, or -1 for none */
    double x      = 0.0;
    double y      = 0.0;

    double vx = 0.0; /* last movement, to tell where the next event should be */
    double vy = 0.0;

    /* Warping only moves the pointer of some of these devices: XWin moves the
       Windows cursor, but a tablet or a VNC client's pointer stays where it is.
       Having found that it does not, try again after a while, in case it was
       misjudged; the wait doubles every time. */
    bool          no_warp      = false;
    int           probe_wait   = 0;
    int           probe_after  = 32;
    bool          warp_pending = false; /* warped, outcome not seen yet */
    bool          warp_echoed  = false; /* the warp came back as an event from the device */
    unsigned long warp_serial  = 0;     /* request number of the warp */
    int           warp_events  = 0;
    double        warp_x       = 0.0;
    double        warp_y       = 0.0;
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
    pos.warp_serial = NextRequest(disp);
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

/* How far (mx, my) is from continuing the last movement, by anything up to a few times over. */
static double
xinput2_motion_score(double mx, double my)
{
    double v2 = (pos.vx * pos.vx) + (pos.vy * pos.vy);
    double t  = (v2 > 0.0) ? std::clamp(((mx * pos.vx) + (my * pos.vy)) / v2, 0.0, 4.0) : 0.0;
    return std::hypot(mx - (t * pos.vx), my - (t * pos.vy));
}

/* Motion of a device that reports where the pointer is, rather than how far it moved. */
static void
xinput2_position(int source, double x, double y, bool can_warp, unsigned long serial)
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

    /* Events carry the number of the last request the server had handled, so
       anything before the warp is plain motion from where the pointer was, no
       matter how late it arrives over a slow remote display. */
    if (pos.warp_pending && ((long) (serial - pos.warp_serial) >= 0)) {
        if ((x == pos.warp_x) && (y == pos.warp_y)) {
            /* The warp coming back from the device (XWin moving the Windows
               cursor), which a tablet moving it can still undo. */
            pos.warp_echoed = true;
            return;
        }

        /* Tell whether the pointer carried on from the warp target or from where
           it was. Several movements can come in as one event, more so when the
           display is slow, and a fast one can go further than the warp did. */
        if (xinput2_motion_score(x - pos.warp_x, y - pos.warp_y) < xinput2_motion_score(dx, dy)) {
            /* The warp moved the device's pointer; carry on from there. */
            dx               = x - pos.warp_x;
            dy               = y - pos.warp_y;
            pos.warp_pending = false;
            pos.probe_after  = 32;
        } else if (pos.warp_echoed || (++pos.warp_events >= 2)) {
            /* The device kept reporting from where it was: stop warping it. */
            pos.warp_pending = false;
            pos.no_warp      = true;
            pos.probe_wait   = pos.probe_after;
            pos.probe_after  = std::min(pos.probe_after * 2, 4096);
        }
    }

    mouse_scalef(dx, dy);
    pos.x  = x;
    pos.y  = y;
    pos.vx = dx;
    pos.vy = dy;

    if (pos.no_warp && (--pos.probe_wait <= 0))
        pos.no_warp = false;

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

/* Whether (x, y) is somewhere the pointer could have gone from (from_x, from_y),
   moving about as fast as it last did. */
static bool
xinput2_near(double x, double y, double from_x, double from_y)
{
    double reach = 64.0 + std::max(std::fabs(pos.vx), std::fabs(pos.vy));
    return (std::fabs(x - from_x) <= reach) && (std::fabs(y - from_y) <= reach);
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
                xinput2_position(rawev->sourceid, x, y, false, rawev->serial);
            return;

        case PointerKind::Pixels:
            /* The same XTEST device can be driven with positions by a VNC server
               and with relative motion by something else, such as Steam Input.
               A negative value can only be motion. Small values far from where
               the pointer was could be either, so check those with the server,
               and give up on positions after a few misses. Anything else is a
               position: on a slow remote display, the server's pointer will
               have moved on by the time it answers, so it cannot tell. */
            if ((present != 3) && (pos.source != rawev->sourceid))
                return;
            x = (present & 1) ? coords[0] : pos.x;
            y = (present & 2) ? coords[1] : pos.y;
            if ((x >= 0.0) && (y >= 0.0)) {
                if ((x > 64.0) || (y > 64.0) || ((pos.source == rawev->sourceid) && xinput2_near(x, y, pos.x, pos.y)) || ((present == 3) && xinput2_is_position(rawev->deviceid, coords, pos.source == rawev->sourceid, pos.x, pos.y))) {
                    info.mismatches = 0;
                    xinput2_position(rawev->sourceid, x, y, true, rawev->serial);
                    return;
                }
                if (++info.mismatches < 3)
                    return; /* the next position covers this one's movement */
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
                info.last_x     = coords[0];
                info.last_y     = coords[1];
                xinput2_position(rawev->sourceid, coords[0], coords[1], false, rawev->serial);
                return;
            }
            break;

        case PointerKind::Unknown:
            /* Raw values the server's pointer went through, from where it was when
               last seen (the previous position, if the previous raw values were
               one) to where it is now, are positions. Anything else, including
               motion along one axis only, is relative motion; except that a large
               positive step may be a position the server's pointer has already
               moved on from, so that is left undecided. */
            if (present == 3) {
                bool is_position = (info.matches > 0) ? xinput2_is_position(rawev->deviceid, coords, true, info.last_x, info.last_y) : xinput2_is_position(rawev->deviceid, coords, have_query, query_x, query_y);
                if (is_position) {
                    info.last_x     = coords[0];
                    info.last_y     = coords[1];
                    info.mismatches = 0;
                    if (++info.matches >= 2)
                        info.kind = PointerKind::Pixels;
                    xinput2_position(rawev->sourceid, coords[0], coords[1], false, rawev->serial);
                    return;
                }
                if ((coords[0] >= 0.0) && (coords[1] >= 0.0) && ((coords[0] > 64.0) || (coords[1] > 64.0)))
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

                        xinput2_position(devev->sourceid, devev->root_x, devev->root_y, true, devev->serial);
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
    /* Also start over here: mouse_capture is set before xinput2_mouse_capture
       runs, so the first events of the next capture could otherwise be taken
       as moving on from where the pointer was during the last one. */
    reset_motion = true;
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
