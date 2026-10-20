/*
 * ei_scroll_fix.c — LD_PRELOAD shim that repairs wheel scrolling for deskflow
 * and Synergy running as a Wayland server through the input-capture portal.
 *
 * Part of ei-scroll-fix:  https://github.com/pingod/ei-scroll-fix
 * SPDX-License-Identifier: MIT
 *
 * Tested with synergy-core v3.8.4 (based on Deskflow v1.26) on Hyprland 0.56.2
 * with a macOS client.  The underlying deskflow bug is still present on master,
 * so deskflow builds are affected as well.
 *
 * THE BUG
 * -------
 * Hyprland (src/managers/input/InputManager.cpp:994-999) does, for every wheel detent:
 *
 *     PROTO::inputCapture->axis(e.axis, e.delta);            // e.delta   = 15  (DEGREES)
 *     if (e.source == 0)
 *         PROTO::inputCapture->axisValue120(e.axis, e.delta); // e.delta again!
 *
 * libei's contract (include/libei.h):
 *   ei_device_scroll_delta()    -> logical pixels
 *   ei_device_scroll_discrete() -> fractions or multiples of 120
 * and explicitly: "do not call ei_device_scroll_discrete() for the same input event".
 *
 * Hyprland therefore (a) emits BOTH channels for one detent and (b) puts 15
 * degrees into the 120-unit channel (8x low).
 *
 * synergy-core's EiScreen keeps ONE remainder pair per ei_device shared by both
 * handlers, in two different units (see deskflow PR #10277, still open), so the
 * +-15 discrete value poisons the fractional-click accumulator and one physical
 * notch degenerates into 120, 2040, 1920, 2040, ... — i.e. 1 then 16-17 whole
 * clicks per notch.  With the mac client's hardcoded 3x boost (removed upstream
 * only by PR #10257) each click posts a 3-line CGEvent, so the user sees
 * ~48-51 lines per notch delivered as 16-17 separate jumps.
 *
 * THE FIX (server side, no rebuild, reversible)
 * -------------------------------------------
 *   1. ei_event_scroll_get_discrete_dx/dy  -> 0     (kill the broken channel;
 *      deskflow's discrete handler then just re-writes an unchanged remainder)
 *   2. ei_event_scroll_get_dx/dy / 1.5      (15 degrees -> 10 "pixels" ->
 *      exactly 1.0 wheel click -> exactly one 120-unit event per detent)
 *
 * Result: perfectly uniform 120 units per notch, hence 3 lines per notch on the
 * mac client (the boost that was meant to emulate a native mac notch).
 *
 * TOUCHPAD SAFETY
 * ---------------
 * Step 2 must NOT slow down trackpad scrolling, whose deltas are arbitrary
 * libinput pixel values.  A wheel detent reaches us as an exact multiple of 15.0
 * (Hyprland passes libinput's angle unmodified on the EIS path), so the divisor
 * is applied only to values that are within 1e-6 of a multiple of 15.0.
 * Everything else — i.e. every trackpad delta — passes through untouched.
 * Set EISFIX_WHEEL_ONLY=0 to disable this guard (divide unconditionally).
 *
 * The local (cursor-on-Linux) path is not affected: EiScreen::fakeMouseWheel
 * calls ei_device_scroll_discrete() — the SEND side — which this shim does not
 * hook.  Verified round-trip: 1 physical notch -> 120 units -> 1 notch locally.
 *
 * TUNABLES (environment):
 *   EISFIX_SMOOTH_DIV=1.5    divisor applied to wheel-notch smooth deltas
 *   EISFIX_DROP_DISCRETE=1   1 = neutralise the discrete channel (default)
 *   EISFIX_WHEEL_ONLY=1      1 = only divide exact multiples of 15.0 (default)
 *   EISFIX_DISABLE=1         1 = do nothing at all (escape hatch)
 *   EISFIX_DEBUG=1           log decisions to stderr
 *   EISFIX_LOG=/path         append decisions to this file (survives the
 *                            service's stderr pipe, so it is the reliable way
 *                            to verify the shim on a real session)
 *
 * Build:  gcc -shared -fPIC -O2 -Wall -Wextra -o ei_scroll_fix.so ei_scroll_fix.c -ldl -lm
 * Use:    LD_PRELOAD=/abs/path/ei_scroll_fix.so synergy-core <args>
 */

#define _GNU_SOURCE
#include <dlfcn.h>
#include <math.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

typedef double  (*fn_d_d)(void *);
typedef int32_t (*fn_i_d)(void *);

static fn_d_d real_dx, real_dy;
static fn_i_d real_ddx, real_ddy;
static double smooth_div = 1.5;
static int    drop_discrete = 1;
static int    wheel_only = 1;
static int    disabled = 0;
static int    debug = 0;
static int    inited = 0;
static char   log_path[512];

/* One wheel detent == 15 degrees == 1/8 of a 120-unit v120 click. */
#define WHEEL_NOTCH 15.0

static void shim_log(const char *fmt, ...)
{
    if (!debug && !log_path[0]) return;

    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);

    if (debug) fprintf(stderr, "[ei_scroll_fix] %s\n", buf);

    if (log_path[0]) {
        /* Keep the log from growing without bound: rotate once past 2 MiB. */
        struct stat st;
        if (stat(log_path, &st) == 0 && st.st_size > 2 * 1024 * 1024) {
            char old[520];
            snprintf(old, sizeof old, "%s.1", log_path);
            rename(log_path, old);
        }
        FILE *f = fopen(log_path, "a");
        if (f) {
            struct timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);
            fprintf(f, "%ld.%03ld [%d] %s\n", (long)ts.tv_sec, ts.tv_nsec / 1000000,
                    (int)getpid(), buf);
            fclose(f);
        }
    }
}

static void init_once(void)
{
    if (inited) return;
    inited = 1;

    const char *e;
    if ((e = getenv("EISFIX_DISABLE")) && *e && *e != '0') disabled = 1;
    if ((e = getenv("EISFIX_DROP_DISCRETE")) && *e) drop_discrete = (*e != '0');
    if ((e = getenv("EISFIX_WHEEL_ONLY")) && *e) wheel_only = (*e != '0');
    if ((e = getenv("EISFIX_DEBUG")) && *e && *e != '0') debug = 1;
    if ((e = getenv("EISFIX_LOG")) && *e) {
        strncpy(log_path, e, sizeof log_path - 1);
        log_path[sizeof log_path - 1] = '\0';
    }
    if ((e = getenv("EISFIX_SMOOTH_DIV"))) {
        double v = atof(e);
        if (v > 0.0) smooth_div = v;
    }

    real_dx  = (fn_d_d)dlsym(RTLD_NEXT, "ei_event_scroll_get_dx");
    real_dy  = (fn_d_d)dlsym(RTLD_NEXT, "ei_event_scroll_get_dy");
    real_ddx = (fn_i_d)dlsym(RTLD_NEXT, "ei_event_scroll_get_discrete_dx");
    real_ddy = (fn_i_d)dlsym(RTLD_NEXT, "ei_event_scroll_get_discrete_dy");

    shim_log("active smooth_div=%.4f drop_discrete=%d wheel_only=%d disabled=%d "
             "real_dx=%p real_dy=%p real_ddx=%p real_ddy=%p",
             smooth_div, drop_discrete, wheel_only, disabled,
             (void *)real_dx, (void *)real_dy, (void *)real_ddx, (void *)real_ddy);
}

/* Is this value a wheel detent (exact multiple of 15.0) rather than a
 * trackpad/continuous pixel delta? */
static int is_wheel_notch(double v)
{
    if (!wheel_only) return 1;
    if (v == 0.0) return 0;
    double n = v / WHEEL_NOTCH;
    double r = nearbyint(n);
    return fabs(n - r) < 1e-6;
}

static double scale_smooth(const char *axis, double v)
{
    if (!is_wheel_notch(v)) {
        if (v != 0.0)
            shim_log("smooth %s %.3f passthrough (continuous)", axis, v);
        return v;
    }
    double r = v / smooth_div;
    shim_log("wheel   %s %.3f -> %.3f", axis, v, r);
    return r;
}

double ei_event_scroll_get_dx(void *ev)
{
    init_once();
    if (!real_dx) return 0.0;
    double v = real_dx(ev);
    if (disabled) return v;
    return scale_smooth("dx", v);
}

double ei_event_scroll_get_dy(void *ev)
{
    init_once();
    if (!real_dy) return 0.0;
    double v = real_dy(ev);
    if (disabled) return v;
    return scale_smooth("dy", v);
}

int32_t ei_event_scroll_get_discrete_dx(void *ev)
{
    init_once();
    if (!real_ddx) return 0;
    if (disabled || !drop_discrete) return real_ddx(ev);
    int32_t raw = real_ddx(ev);
    if (raw) shim_log("discrete dx %d -> 0", raw);
    return 0;
}

int32_t ei_event_scroll_get_discrete_dy(void *ev)
{
    init_once();
    if (!real_ddy) return 0;
    if (disabled || !drop_discrete) return real_ddy(ev);
    int32_t raw = real_ddy(ev);
    if (raw) shim_log("discrete dy %d -> 0", raw);
    return 0;
}
