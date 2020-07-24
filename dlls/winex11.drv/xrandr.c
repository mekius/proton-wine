/*
 * Wine X11drv Xrandr interface
 *
 * Copyright 2003 Alexander James Pasadyn
 * Copyright 2012 Henri Verbeet for CodeWeavers
 * Copyright 2019 Zhiyi Zhang for CodeWeavers
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

#include "config.h"
#include "wine/port.h"

#define NONAMELESSSTRUCT
#define NONAMELESSUNION

#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(xrandr);
#ifdef HAVE_XRRGETSCREENRESOURCES
WINE_DECLARE_DEBUG_CHANNEL(winediag);
#endif

#ifdef SONAME_LIBXRANDR

#include <assert.h>
#include <X11/Xlib.h>
#include <X11/extensions/Xrandr.h>
#include "x11drv.h"

#define VK_NO_PROTOTYPES
#define WINE_VK_HOST

#include "wine/heap.h"
#include "wine/unicode.h"
#include "wine/vulkan.h"
#include "wine/vulkan_driver.h"

static void *xrandr_handle;

#define MAKE_FUNCPTR(f) static typeof(f) * p##f;
MAKE_FUNCPTR(XRRConfigCurrentConfiguration)
MAKE_FUNCPTR(XRRConfigCurrentRate)
MAKE_FUNCPTR(XRRFreeScreenConfigInfo)
MAKE_FUNCPTR(XRRGetScreenInfo)
MAKE_FUNCPTR(XRRQueryExtension)
MAKE_FUNCPTR(XRRQueryVersion)
MAKE_FUNCPTR(XRRRates)
MAKE_FUNCPTR(XRRSetScreenConfig)
MAKE_FUNCPTR(XRRSetScreenConfigAndRate)
MAKE_FUNCPTR(XRRSizes)

#ifdef HAVE_XRRGETSCREENRESOURCES
MAKE_FUNCPTR(XRRFreeCrtcInfo)
MAKE_FUNCPTR(XRRFreeOutputInfo)
MAKE_FUNCPTR(XRRFreeScreenResources)
MAKE_FUNCPTR(XRRGetCrtcInfo)
MAKE_FUNCPTR(XRRGetOutputInfo)
MAKE_FUNCPTR(XRRGetScreenResources)
MAKE_FUNCPTR(XRRGetScreenSizeRange)
MAKE_FUNCPTR(XRRSetCrtcConfig)
MAKE_FUNCPTR(XRRSetScreenSize)
static typeof(XRRGetScreenResources) *pXRRGetScreenResourcesCurrent;
static RRMode *xrandr12_modes;
static int primary_crtc;
#endif

#ifdef HAVE_XRRGETPROVIDERRESOURCES
MAKE_FUNCPTR(XRRSelectInput)
MAKE_FUNCPTR(XRRGetOutputPrimary)
MAKE_FUNCPTR(XRRGetProviderResources)
MAKE_FUNCPTR(XRRFreeProviderResources)
MAKE_FUNCPTR(XRRGetProviderInfo)
MAKE_FUNCPTR(XRRFreeProviderInfo)
#endif

#undef MAKE_FUNCPTR

static struct x11drv_mode_info *dd_modes;
static SizeID *xrandr10_modes;
static unsigned int xrandr_mode_count;
static int xrandr_current_mode = -1;

static int load_xrandr(void)
{
    int r = 0;

    if (dlopen(SONAME_LIBXRENDER, RTLD_NOW|RTLD_GLOBAL) &&
        (xrandr_handle = dlopen(SONAME_LIBXRANDR, RTLD_NOW)))
    {

#define LOAD_FUNCPTR(f) \
        if((p##f = dlsym(xrandr_handle, #f)) == NULL) goto sym_not_found

        LOAD_FUNCPTR(XRRConfigCurrentConfiguration);
        LOAD_FUNCPTR(XRRConfigCurrentRate);
        LOAD_FUNCPTR(XRRFreeScreenConfigInfo);
        LOAD_FUNCPTR(XRRGetScreenInfo);
        LOAD_FUNCPTR(XRRQueryExtension);
        LOAD_FUNCPTR(XRRQueryVersion);
        LOAD_FUNCPTR(XRRRates);
        LOAD_FUNCPTR(XRRSetScreenConfig);
        LOAD_FUNCPTR(XRRSetScreenConfigAndRate);
        LOAD_FUNCPTR(XRRSizes);
        r = 1;

#ifdef HAVE_XRRGETSCREENRESOURCES
        LOAD_FUNCPTR(XRRFreeCrtcInfo);
        LOAD_FUNCPTR(XRRFreeOutputInfo);
        LOAD_FUNCPTR(XRRFreeScreenResources);
        LOAD_FUNCPTR(XRRGetCrtcInfo);
        LOAD_FUNCPTR(XRRGetOutputInfo);
        LOAD_FUNCPTR(XRRGetScreenResources);
        LOAD_FUNCPTR(XRRGetScreenSizeRange);
        LOAD_FUNCPTR(XRRSetCrtcConfig);
        LOAD_FUNCPTR(XRRSetScreenSize);
        r = 2;
#endif

#ifdef HAVE_XRRGETPROVIDERRESOURCES
        LOAD_FUNCPTR(XRRSelectInput);
        LOAD_FUNCPTR(XRRGetOutputPrimary);
        LOAD_FUNCPTR(XRRGetProviderResources);
        LOAD_FUNCPTR(XRRFreeProviderResources);
        LOAD_FUNCPTR(XRRGetProviderInfo);
        LOAD_FUNCPTR(XRRFreeProviderInfo);
        r = 4;
#endif

#undef LOAD_FUNCPTR

sym_not_found:
        if (!r)  TRACE("Unable to load function ptrs from XRandR library\n");
    }
    return r;
}

static int XRandRErrorHandler(Display *dpy, XErrorEvent *event, void *arg)
{
    return 1;
}

static int xrandr10_get_current_mode(void)
{
    SizeID size;
    Rotation rot;
    XRRScreenConfiguration *sc;
    short rate;
    unsigned int i;
    int res = -1;

    if (xrandr_current_mode != -1)
        return xrandr_current_mode;

    sc = pXRRGetScreenInfo (gdi_display, DefaultRootWindow( gdi_display ));
    size = pXRRConfigCurrentConfiguration (sc, &rot);
    rate = pXRRConfigCurrentRate (sc);
    pXRRFreeScreenConfigInfo(sc);

    for (i = 0; i < xrandr_mode_count; ++i)
    {
        if (xrandr10_modes[i] == size && dd_modes[i].refresh_rate == rate)
        {
            res = i;
            break;
        }
    }
    if (res == -1)
    {
        ERR("In unknown mode, returning default\n");
        return 0;
    }

    xrandr_current_mode = res;
    return res;
}

static LONG xrandr10_set_current_mode( int mode )
{
    SizeID size;
    Rotation rot;
    Window root;
    XRRScreenConfiguration *sc;
    Status stat;
    short rate;

    root = DefaultRootWindow( gdi_display );
    sc = pXRRGetScreenInfo (gdi_display, root);
    pXRRConfigCurrentConfiguration (sc, &rot);
    mode = mode % xrandr_mode_count;

    TRACE("Changing Resolution to %dx%d @%d Hz\n",
          dd_modes[mode].width,
          dd_modes[mode].height,
          dd_modes[mode].refresh_rate);

    size = xrandr10_modes[mode];
    rate = dd_modes[mode].refresh_rate;

    if (rate)
        stat = pXRRSetScreenConfigAndRate( gdi_display, sc, root, size, rot, rate, CurrentTime );
    else
        stat = pXRRSetScreenConfig( gdi_display, sc, root, size, rot, CurrentTime );

    pXRRFreeScreenConfigInfo(sc);

    if (stat == RRSetConfigSuccess)
    {
        xrandr_current_mode = mode;
        X11DRV_DisplayDevices_Update( TRUE );
        return DISP_CHANGE_SUCCESSFUL;
    }

    ERR("Resolution change not successful -- perhaps display has changed?\n");
    return DISP_CHANGE_FAILED;
}

static void xrandr10_init_modes(void)
{
    XRRScreenSize *sizes;
    int sizes_count;
    int i, j, nmodes = 0;

    ERR("xrandr 1.2 support required\n");
    return;

    sizes = pXRRSizes( gdi_display, DefaultScreen(gdi_display), &sizes_count );
    if (sizes_count <= 0) return;

    TRACE("XRandR: found %d sizes.\n", sizes_count);
    for (i = 0; i < sizes_count; ++i)
    {
        int rates_count;
        short *rates;

        rates = pXRRRates( gdi_display, DefaultScreen(gdi_display), i, &rates_count );
        TRACE("- at %d: %dx%d (%d rates):", i, sizes[i].width, sizes[i].height, rates_count);
        if (rates_count)
        {
            nmodes += rates_count;
            for (j = 0; j < rates_count; ++j)
            {
                if (j > 0)
                    TRACE(",");
                TRACE("  %d", rates[j]);
            }
        }
        else
        {
            ++nmodes;
            TRACE(" <default>");
        }
        TRACE(" Hz\n");
    }

    TRACE("XRandR modes: count=%d\n", nmodes);

    if (!(xrandr10_modes = HeapAlloc( GetProcessHeap(), 0, sizeof(*xrandr10_modes) * nmodes )))
    {
        ERR("Failed to allocate xrandr mode info array.\n");
        return;
    }

    dd_modes = X11DRV_Settings_SetHandlers( "XRandR 1.0",
                                            xrandr10_get_current_mode,
                                            xrandr10_set_current_mode,
                                            nmodes, 1 );

    xrandr_mode_count = 0;
    for (i = 0; i < sizes_count; ++i)
    {
        int rates_count;
        short *rates;

        rates = pXRRRates( gdi_display, DefaultScreen(gdi_display), i, &rates_count );

        if (rates_count)
        {
            for (j = 0; j < rates_count; ++j)
            {
                X11DRV_Settings_AddOneMode( sizes[i].width, sizes[i].height, 0, rates[j] );
                xrandr10_modes[xrandr_mode_count++] = i;
            }
        }
        else
        {
            X11DRV_Settings_AddOneMode( sizes[i].width, sizes[i].height, 0, 0 );
            xrandr10_modes[xrandr_mode_count++] = i;
        }
    }

    X11DRV_Settings_AddDepthModes();
    nmodes = X11DRV_Settings_GetModeCount();

    TRACE("Available DD modes: count=%d\n", nmodes);
    TRACE("Enabling XRandR\n");
}

#ifdef HAVE_XRRGETSCREENRESOURCES

static XRRScreenResources *xrandr_get_screen_resources(void)
{
    XRRScreenResources *resources = pXRRGetScreenResourcesCurrent( gdi_display, root_window );
    if (resources && !resources->ncrtc)
    {
        pXRRFreeScreenResources( resources );
        resources = pXRRGetScreenResources( gdi_display, root_window );
    }

    if (!resources)
        ERR("Failed to get screen resources.\n");
    return resources;
}

static int xrandr12_get_current_mode(void)
{
    XRRScreenResources *resources;
    XRRCrtcInfo *crtc_info;
    int i, ret = -1;

    if (xrandr_current_mode != -1)
        return xrandr_current_mode;

    if (!(resources = pXRRGetScreenResourcesCurrent( gdi_display, root_window )))
    {
        ERR("Failed to get screen resources.\n");
        return 0;
    }

    if (resources->ncrtc <= primary_crtc ||
        !(crtc_info = pXRRGetCrtcInfo( gdi_display, resources, resources->crtcs[primary_crtc] )))
    {
        pXRRFreeScreenResources( resources );
        ERR("Failed to get CRTC info.\n");
        return 0;
    }

    TRACE("CRTC %d: mode %#lx, %ux%u+%d+%d.\n", primary_crtc, crtc_info->mode,
          crtc_info->width, crtc_info->height, crtc_info->x, crtc_info->y);

    for (i = 0; i < xrandr_mode_count; ++i)
    {
        if (xrandr12_modes[i] == crtc_info->mode)
        {
            ret = i;
            break;
        }
    }

    pXRRFreeCrtcInfo( crtc_info );
    pXRRFreeScreenResources( resources );

    if (ret == -1)
    {
        ERR("Unknown mode, returning default.\n");
        return 0;
    }

    xrandr_current_mode = ret;
    return ret;
}

static void get_screen_size( XRRScreenResources *resources, unsigned int *width, unsigned int *height )
{
    int min_width = 0, min_height = 0, max_width, max_height;
    XRRCrtcInfo *crtc_info;
    int i;

    pXRRGetScreenSizeRange( gdi_display, root_window, &min_width, &min_height, &max_width, &max_height );
    *width = min_width;
    *height = min_height;

    for (i = 0; i < resources->ncrtc; ++i)
    {
        if (!(crtc_info = pXRRGetCrtcInfo( gdi_display, resources, resources->crtcs[i] )))
            continue;

        if (crtc_info->mode != None)
        {
            *width = max(*width, crtc_info->x + crtc_info->width);
            *height = max(*height, crtc_info->y + crtc_info->height);
        }

        pXRRFreeCrtcInfo( crtc_info );
    }
}

static void set_screen_size( int width, int height )
{
    int screen = default_visual.screen;
    int mm_width, mm_height;

    mm_width = width * DisplayWidthMM( gdi_display, screen ) / DisplayWidth( gdi_display, screen );
    mm_height = height * DisplayHeightMM( gdi_display, screen ) / DisplayHeight( gdi_display, screen );
    pXRRSetScreenSize( gdi_display, root_window, width, height, mm_width, mm_height );
}

static LONG xrandr12_set_current_mode( int mode )
{
    unsigned int screen_width, screen_height;
    Status status = RRSetConfigFailed;
    XRRScreenResources *resources;
    XRRCrtcInfo *crtc_info;

    mode = mode % xrandr_mode_count;

    if (!(resources = pXRRGetScreenResourcesCurrent( gdi_display, root_window )))
    {
        ERR("Failed to get screen resources.\n");
        return DISP_CHANGE_FAILED;
    }

    if (resources->ncrtc <= primary_crtc ||
        !(crtc_info = pXRRGetCrtcInfo( gdi_display, resources, resources->crtcs[primary_crtc] )))
    {
        pXRRFreeScreenResources( resources );
        ERR("Failed to get CRTC info.\n");
        return DISP_CHANGE_FAILED;
    }

    TRACE("CRTC %d: mode %#lx, %ux%u+%d+%d.\n", primary_crtc, crtc_info->mode,
          crtc_info->width, crtc_info->height, crtc_info->x, crtc_info->y);

    /* According to the RandR spec, the entire CRTC must fit inside the screen.
     * Since we use the union of all enabled CRTCs to determine the necessary
     * screen size, this might involve shrinking the screen, so we must disable
     * the CRTC in question first. */

    XGrabServer( gdi_display );

    status = pXRRSetCrtcConfig( gdi_display, resources, resources->crtcs[primary_crtc],
                                CurrentTime, crtc_info->x, crtc_info->y, None,
                                crtc_info->rotation, NULL, 0 );
    if (status != RRSetConfigSuccess)
    {
        XUngrabServer( gdi_display );
        XFlush( gdi_display );
        ERR("Failed to disable CRTC.\n");
        pXRRFreeCrtcInfo( crtc_info );
        pXRRFreeScreenResources( resources );
        return DISP_CHANGE_FAILED;
    }

    get_screen_size( resources, &screen_width, &screen_height );
    screen_width = max( screen_width, crtc_info->x + dd_modes[mode].width );
    screen_height = max( screen_height, crtc_info->y + dd_modes[mode].height );
    set_screen_size( screen_width, screen_height );

    status = pXRRSetCrtcConfig( gdi_display, resources, resources->crtcs[primary_crtc],
                                CurrentTime, crtc_info->x, crtc_info->y, xrandr12_modes[mode],
                                crtc_info->rotation, crtc_info->outputs, crtc_info->noutput );

    XUngrabServer( gdi_display );
    XFlush( gdi_display );

    pXRRFreeCrtcInfo( crtc_info );
    pXRRFreeScreenResources( resources );

    if (status != RRSetConfigSuccess)
    {
        ERR("Resolution change not successful -- perhaps display has changed?\n");
        return DISP_CHANGE_FAILED;
    }

    xrandr_current_mode = mode;
    X11DRV_DisplayDevices_Update( TRUE );
    return DISP_CHANGE_SUCCESSFUL;
}

static XRRCrtcInfo *xrandr12_get_primary_crtc_info( XRRScreenResources *resources, int *crtc_idx )
{
    XRRCrtcInfo *crtc_info;
    int i;

    for (i = 0; i < resources->ncrtc; ++i)
    {
        crtc_info = pXRRGetCrtcInfo( gdi_display, resources, resources->crtcs[i] );
        if (!crtc_info || crtc_info->mode == None)
        {
            pXRRFreeCrtcInfo( crtc_info );
            continue;
        }

        *crtc_idx = i;
        return crtc_info;
    }

    return NULL;
}

static unsigned int get_frequency( const XRRModeInfo *mode )
{
    unsigned int dots = mode->hTotal * mode->vTotal;

    if (!dots)
        return 0;

    if (mode->modeFlags & RR_DoubleScan)
        dots *= 2;
    if (mode->modeFlags & RR_Interlace)
        dots /= 2;

    return (mode->dotClock + dots / 2) / dots;
}

static int xrandr12_init_modes(void)
{
    unsigned int only_one_resolution = 1, mode_count, primary_width, primary_height;
    XRRScreenResources *resources;
    XRROutputInfo *output_info;
    XRRModeInfo *primary_mode = NULL;
    XRRCrtcInfo *crtc_info;
    unsigned int primary_refresh;
    int ret = -1;
    int i, j;

    if (!(resources = xrandr_get_screen_resources()))
        return ret;

    if (!(crtc_info = xrandr12_get_primary_crtc_info( resources, &primary_crtc )))
    {
        pXRRFreeScreenResources( resources );
        ERR("Failed to get primary CRTC info.\n");
        return ret;
    }

    for (i = 0; i < resources->nmode; ++i)
    {
        if (resources->modes[i].id == crtc_info->mode)
        {
            primary_mode = &resources->modes[i];
        }
    }

    TRACE("CRTC %d: mode %#lx, %ux%u+%d+%d.\n", primary_crtc, crtc_info->mode,
          crtc_info->width, crtc_info->height, crtc_info->x, crtc_info->y);

    if (!crtc_info->noutput || !(output_info = pXRRGetOutputInfo( gdi_display, resources, crtc_info->outputs[0] )))
    {
        pXRRFreeCrtcInfo( crtc_info );
        pXRRFreeScreenResources( resources );
        ERR("Failed to get output info.\n");
        return ret;
    }

    TRACE("OUTPUT 0: name %s.\n", debugstr_a(output_info->name));

    if (!output_info->nmode)
    {
        WARN("Output has no modes.\n");
        goto done;
    }

    if (!(xrandr12_modes = HeapAlloc( GetProcessHeap(), 0, sizeof(*xrandr12_modes) * output_info->nmode )))
    {
        ERR("Failed to allocate xrandr mode info array.\n");
        goto done;
    }

    dd_modes = X11DRV_Settings_SetHandlers( "XRandR 1.2",
                                            NULL,
                                            NULL,
                                            output_info->nmode, 1 );

    if(primary_mode)
    {
        primary_refresh = get_frequency(primary_mode);
        primary_width = primary_mode->width;
        primary_height = primary_mode->height;
    }
    else
    {
        WARN("Couldn't find primary mode! defaulting to 60 Hz\n");
        primary_refresh = 60;
        primary_width = crtc_info->width;
        primary_height = crtc_info->height;
    }

    if((crtc_info->rotation & RR_Rotate_90) ||
            (crtc_info->rotation & RR_Rotate_270))
    {
        unsigned int tmp = primary_width;
        primary_width = primary_height;
        primary_height = tmp;
    }

    xrandr_mode_count = 0;
    for (i = 0; i < output_info->nmode; ++i)
    {
        for (j = 0; j < resources->nmode; ++j)
        {
            XRRModeInfo *mode = &resources->modes[j];

            if (mode->id == output_info->modes[i])
            {

                XRRModeInfo rotated_mode = *mode;
                if((crtc_info->rotation & RR_Rotate_90) ||
                        (crtc_info->rotation & RR_Rotate_270))
                {
                    unsigned int tmp = rotated_mode.width;
                    rotated_mode.width = rotated_mode.height;
                    rotated_mode.height = tmp;
                }

                if(rotated_mode.width <= primary_width &&
                        rotated_mode.height <= primary_height &&
                        X11DRV_Settings_AddOneMode( rotated_mode.width, rotated_mode.height, 0, primary_refresh ))
                {
                    TRACE("Added mode %#lx: %ux%u@%u.\n", rotated_mode.id, rotated_mode.width, rotated_mode.height, primary_refresh);
                    xrandr12_modes[xrandr_mode_count++] = rotated_mode.id;
                }
                break;
            }
        }
    }

    mode_count = X11DRV_Settings_GetModeCount();
    for (i = 1; i < mode_count; ++i)
    {
        if (dd_modes[i].width != dd_modes[0].width || dd_modes[i].height != dd_modes[0].height)
        {
            only_one_resolution = 0;
            break;
        }
    }

    X11DRV_Settings_SetRealMode(primary_width, primary_height);

    X11DRV_Settings_AddDepthModes();
    ret = 0;

done:
    pXRRFreeOutputInfo( output_info );
    pXRRFreeCrtcInfo( crtc_info );
    pXRRFreeScreenResources( resources );
    return ret;
}

#endif /* HAVE_XRRGETSCREENRESOURCES */

#ifdef HAVE_XRRGETPROVIDERRESOURCES

static RECT get_primary_rect( XRRScreenResources *resources )
{
    XRROutputInfo *output_info = NULL;
    XRRCrtcInfo *crtc_info = NULL;
    RROutput primary_output;
    RECT primary_rect = {0};
    RECT first_rect = {0};
    INT i;

    primary_output = pXRRGetOutputPrimary( gdi_display, root_window );
    if (!primary_output)
        goto fallback;

    output_info = pXRRGetOutputInfo( gdi_display, resources, primary_output );
    if (!output_info || output_info->connection != RR_Connected || !output_info->crtc)
        goto fallback;

    crtc_info = pXRRGetCrtcInfo( gdi_display, resources, output_info->crtc );
    if (!crtc_info || !crtc_info->mode)
        goto fallback;

    SetRect( &primary_rect, crtc_info->x, crtc_info->y, crtc_info->x + crtc_info->width, crtc_info->y + crtc_info->height );
    pXRRFreeCrtcInfo( crtc_info );
    pXRRFreeOutputInfo( output_info );
    return primary_rect;

/* Fallback when XRandR primary output is a disconnected output.
 * Try to find a crtc with (x, y) being (0, 0). If it's found then get the primary rect from that crtc,
 * otherwise use the first active crtc to get the primary rect */
fallback:
    if (crtc_info)
        pXRRFreeCrtcInfo( crtc_info );
    if (output_info)
        pXRRFreeOutputInfo( output_info );

    WARN("Primary is set to a disconnected XRandR output.\n");
    for (i = 0; i < resources->ncrtc; ++i)
    {
        crtc_info = pXRRGetCrtcInfo( gdi_display, resources, resources->crtcs[i] );
        if (!crtc_info)
            continue;

        if (!crtc_info->mode)
        {
            pXRRFreeCrtcInfo( crtc_info );
            continue;
        }

        if (!crtc_info->x && !crtc_info->y)
        {
            SetRect( &primary_rect, 0, 0, crtc_info->width, crtc_info->height );
            pXRRFreeCrtcInfo( crtc_info );
            break;
        }

        if (IsRectEmpty( &first_rect ))
            SetRect( &first_rect, crtc_info->x, crtc_info->y,
                     crtc_info->x + crtc_info->width, crtc_info->y + crtc_info->height );

        pXRRFreeCrtcInfo( crtc_info );
    }

    return IsRectEmpty( &primary_rect ) ? first_rect : primary_rect;
}

static BOOL is_crtc_primary( RECT primary, const XRRCrtcInfo *crtc )
{
    return crtc &&
           crtc->mode &&
           crtc->x == primary.left &&
           crtc->y == primary.top &&
           crtc->x + crtc->width == primary.right &&
           crtc->y + crtc->height == primary.bottom;
}

VK_DEFINE_NON_DISPATCHABLE_HANDLE(VkDisplayKHR)

static BOOL get_gpu_properties_from_vulkan( struct x11drv_gpu *gpu, const XRRProviderInfo *provider_info )
{
    static const char *extensions[] =
    {
        VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME,
        "VK_EXT_acquire_xlib_display",
        "VK_EXT_direct_mode_display",
    };
    const struct vulkan_funcs *vulkan_funcs = get_vulkan_driver( WINE_VULKAN_DRIVER_VERSION );
    VkResult (*pvkGetRandROutputDisplayEXT)( VkPhysicalDevice, Display *, RROutput, VkDisplayKHR * );
    PFN_vkGetPhysicalDeviceProperties2 pvkGetPhysicalDeviceProperties2;
    PFN_vkEnumeratePhysicalDevices pvkEnumeratePhysicalDevices;
    uint32_t device_count, device_idx, output_idx;
    VkPhysicalDevice *vk_physical_devices = NULL;
    VkPhysicalDeviceProperties2 properties2;
    VkInstanceCreateInfo create_info;
    VkPhysicalDeviceIDProperties id;
    VkInstance vk_instance = NULL;
    VkDisplayKHR vk_display;
    BOOL ret = FALSE;
    VkResult vr;

    if (!vulkan_funcs)
        goto done;

    memset( &create_info, 0, sizeof(create_info) );
    create_info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    create_info.enabledExtensionCount = ARRAY_SIZE(extensions);
    create_info.ppEnabledExtensionNames = extensions;

    vr = vulkan_funcs->p_vkCreateInstance( &create_info, NULL, &vk_instance );
    if (vr != VK_SUCCESS)
    {
        WARN("Failed to create a Vulkan instance, vr %d.\n", vr);
        goto done;
    }

#define LOAD_VK_FUNC(f)                                                             \
    if (!(p##f = (void *)vulkan_funcs->p_vkGetInstanceProcAddr( vk_instance, #f ))) \
    {                                                                               \
        WARN("Failed to load " #f ".\n");                                           \
        goto done;                                                                  \
    }

    LOAD_VK_FUNC(vkEnumeratePhysicalDevices)
    LOAD_VK_FUNC(vkGetPhysicalDeviceProperties2)
    LOAD_VK_FUNC(vkGetRandROutputDisplayEXT)
#undef LOAD_VK_FUNC

    vr = pvkEnumeratePhysicalDevices( vk_instance, &device_count, NULL );
    if (vr != VK_SUCCESS || !device_count)
    {
        WARN("No Vulkan device found, vr %d, device_count %d.\n", vr, device_count);
        goto done;
    }

    if (!(vk_physical_devices = heap_calloc( device_count, sizeof(*vk_physical_devices) )))
        goto done;

    vr = pvkEnumeratePhysicalDevices( vk_instance, &device_count, vk_physical_devices );
    if (vr != VK_SUCCESS)
    {
        WARN("vkEnumeratePhysicalDevices failed, vr %d.\n", vr);
        goto done;
    }

    for (device_idx = 0; device_idx < device_count; ++device_idx)
    {
        for (output_idx = 0; output_idx < provider_info->noutputs; ++output_idx)
        {
            X11DRV_expect_error( gdi_display, XRandRErrorHandler, NULL );
            vr = pvkGetRandROutputDisplayEXT( vk_physical_devices[device_idx], gdi_display,
                                              provider_info->outputs[output_idx], &vk_display );
            XSync( gdi_display, FALSE );
            if (X11DRV_check_error() || vr != VK_SUCCESS || vk_display == VK_NULL_HANDLE)
                continue;

            memset( &id, 0, sizeof(id) );
            id.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES;
            properties2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
            properties2.pNext = &id;

            pvkGetPhysicalDeviceProperties2( vk_physical_devices[device_idx], &properties2 );
            memcpy( &gpu->vulkan_uuid, id.deviceUUID, sizeof(id.deviceUUID) );
            /* Ignore Khronos vendor IDs */
            if (properties2.properties.vendorID < 0x10000)
            {
                gpu->vendor_id = properties2.properties.vendorID;
                gpu->device_id = properties2.properties.deviceID;
            }
            MultiByteToWideChar( CP_UTF8, 0, properties2.properties.deviceName, -1, gpu->name, ARRAY_SIZE(gpu->name) );
            ret = TRUE;
            goto done;
        }
    }

done:
    heap_free( vk_physical_devices );
    if (vk_instance)
        vulkan_funcs->p_vkDestroyInstance( vk_instance, NULL );
    return ret;
}

/* Get a list of GPUs reported by XRandR 1.4. Set get_properties to FALSE if GPU properties are
 * not needed to avoid unnecessary querying */
static BOOL xrandr14_get_gpus2( struct x11drv_gpu **new_gpus, int *count, BOOL get_properties )
{
    static const WCHAR wine_adapterW[] = {'W','i','n','e',' ','A','d','a','p','t','e','r',0};
    struct x11drv_gpu *gpus = NULL;
    XRRScreenResources *screen_resources = NULL;
    XRRProviderResources *provider_resources = NULL;
    XRRProviderInfo *provider_info = NULL;
    XRRCrtcInfo *crtc_info = NULL;
    INT primary_provider = -1;
    RECT primary_rect;
    BOOL ret = FALSE;
    INT i, j;

    screen_resources = xrandr_get_screen_resources();
    if (!screen_resources)
        goto done;

    provider_resources = pXRRGetProviderResources( gdi_display, root_window );
    if (!provider_resources)
        goto done;

    gpus = heap_calloc( provider_resources->nproviders ? provider_resources->nproviders : 1, sizeof(*gpus) );
    if (!gpus)
        goto done;

    /* Some XRandR implementations don't support providers.
     * In this case, report a fake one to try searching adapters in screen resources */
    if (!provider_resources->nproviders)
    {
        WARN("XRandR implementation doesn't report any providers, faking one.\n");
        lstrcpyW( gpus[0].name, wine_adapterW );
        *new_gpus = gpus;
        *count = 1;
        ret = TRUE;
        goto done;
    }

    primary_rect = get_primary_rect( screen_resources );
    for (i = 0; i < provider_resources->nproviders; ++i)
    {
        provider_info = pXRRGetProviderInfo( gdi_display, screen_resources, provider_resources->providers[i] );
        if (!provider_info)
            goto done;

        /* Find primary provider */
        for (j = 0; primary_provider == -1 && j < provider_info->ncrtcs; ++j)
        {
            crtc_info = pXRRGetCrtcInfo( gdi_display, screen_resources, provider_info->crtcs[j] );
            if (!crtc_info)
                continue;

            if (is_crtc_primary( primary_rect, crtc_info ))
            {
                primary_provider = i;
                pXRRFreeCrtcInfo( crtc_info );
                break;
            }

            pXRRFreeCrtcInfo( crtc_info );
        }

        gpus[i].id = provider_resources->providers[i];
        if (get_properties)
        {
            if (!get_gpu_properties_from_vulkan( &gpus[i], provider_info ))
                MultiByteToWideChar( CP_UTF8, 0, provider_info->name, -1, gpus[i].name, ARRAY_SIZE(gpus[i].name) );
            /* FIXME: Add an alternate method of getting PCI IDs, for systems that don't support Vulkan */
        }
        pXRRFreeProviderInfo( provider_info );
    }

    /* Make primary GPU the first */
    if (primary_provider > 0)
    {
        struct x11drv_gpu tmp = gpus[0];
        gpus[0] = gpus[primary_provider];
        gpus[primary_provider] = tmp;
    }

    *new_gpus = gpus;
    *count = provider_resources->nproviders;
    ret = TRUE;
done:
    if (provider_resources)
        pXRRFreeProviderResources( provider_resources );
    if (screen_resources)
        pXRRFreeScreenResources( screen_resources );
    if (!ret)
    {
        heap_free( gpus );
        ERR("Failed to get gpus\n");
    }
    return ret;
}

static BOOL xrandr14_get_gpus( struct x11drv_gpu **new_gpus, int *count )
{
    return xrandr14_get_gpus2( new_gpus, count, TRUE );
}

static void xrandr14_free_gpus( struct x11drv_gpu *gpus )
{
    heap_free( gpus );
}

static BOOL xrandr14_get_adapters( ULONG_PTR gpu_id, struct x11drv_adapter **new_adapters, int *count )
{
    struct x11drv_adapter *adapters = NULL;
    XRRScreenResources *screen_resources = NULL;
    XRRProviderInfo *provider_info = NULL;
    XRRCrtcInfo *enum_crtc_info, *crtc_info = NULL;
    XRROutputInfo *enum_output_info, *output_info = NULL;
    RROutput *outputs;
    INT crtc_count, output_count;
    INT primary_adapter = 0;
    INT adapter_count = 0;
    BOOL mirrored, detached;
    RECT primary_rect;
    BOOL ret = FALSE;
    INT i, j;

    screen_resources = xrandr_get_screen_resources();
    if (!screen_resources)
        goto done;

    if (gpu_id)
    {
        provider_info = pXRRGetProviderInfo( gdi_display, screen_resources, gpu_id );
        if (!provider_info)
            goto done;

        crtc_count = provider_info->ncrtcs;
        output_count = provider_info->noutputs;
        outputs = provider_info->outputs;
    }
    /* Fake provider id, search adapters in screen resources */
    else
    {
        crtc_count = screen_resources->ncrtc;
        output_count = screen_resources->noutput;
        outputs = screen_resources->outputs;
    }

    /* Actual adapter count could be less */
    adapters = heap_calloc( crtc_count, sizeof(*adapters) );
    if (!adapters)
        goto done;

    primary_rect = get_primary_rect( screen_resources );
    for (i = 0; i < output_count; ++i)
    {
        output_info = pXRRGetOutputInfo( gdi_display, screen_resources, outputs[i] );
        if (!output_info)
            goto done;

        /* Only connected output are considered as monitors */
        if (output_info->connection != RR_Connected)
        {
            pXRRFreeOutputInfo( output_info );
            output_info = NULL;
            continue;
        }

        /* Connected output doesn't mean the output is attached to a crtc */
        detached = FALSE;
        if (output_info->crtc)
        {
            crtc_info = pXRRGetCrtcInfo( gdi_display, screen_resources, output_info->crtc );
            if (!crtc_info)
                goto done;
        }

        if (!output_info->crtc || !crtc_info->mode)
            detached = TRUE;

        /* Ignore mirroring output replicas because mirrored monitors are under the same adapter */
        mirrored = FALSE;
        if (!detached)
        {
            for (j = 0; j < screen_resources->noutput; ++j)
            {
                enum_output_info = pXRRGetOutputInfo( gdi_display, screen_resources, screen_resources->outputs[j] );
                if (!enum_output_info)
                    continue;

                if (enum_output_info->connection != RR_Connected || !enum_output_info->crtc)
                {
                    pXRRFreeOutputInfo( enum_output_info );
                    continue;
                }

                enum_crtc_info = pXRRGetCrtcInfo( gdi_display, screen_resources, enum_output_info->crtc );
                pXRRFreeOutputInfo( enum_output_info );
                if (!enum_crtc_info)
                    continue;

                /* Some outputs may have the same coordinates, aka mirrored. Choose the output with
                 * the lowest value as primary and the rest will then be replicas in a mirroring set */
                if (crtc_info->x == enum_crtc_info->x &&
                    crtc_info->y == enum_crtc_info->y &&
                    crtc_info->width == enum_crtc_info->width &&
                    crtc_info->height == enum_crtc_info->height &&
                    outputs[i] > screen_resources->outputs[j])
                {
                    mirrored = TRUE;
                    pXRRFreeCrtcInfo( enum_crtc_info );
                    break;
                }

                pXRRFreeCrtcInfo( enum_crtc_info );
            }
        }

        if (!mirrored || detached)
        {
            /* Use RROutput as adapter id. The reason of not using RRCrtc is that we need to detect inactive but
             * attached monitors */
            adapters[adapter_count].id = outputs[i];
            if (!detached)
                adapters[adapter_count].state_flags |= DISPLAY_DEVICE_ATTACHED_TO_DESKTOP;
            if (is_crtc_primary( primary_rect, crtc_info ))
            {
                adapters[adapter_count].state_flags |= DISPLAY_DEVICE_PRIMARY_DEVICE;
                primary_adapter = adapter_count;
            }

            ++adapter_count;
        }

        pXRRFreeOutputInfo( output_info );
        output_info = NULL;
        if (crtc_info)
        {
            pXRRFreeCrtcInfo( crtc_info );
            crtc_info = NULL;
        }
    }

    /* Make primary adapter the first */
    if (primary_adapter)
    {
        struct x11drv_adapter tmp = adapters[0];
        adapters[0] = adapters[primary_adapter];
        adapters[primary_adapter] = tmp;
    }

    *new_adapters = adapters;
    *count = adapter_count;
    ret = TRUE;
done:
    if (screen_resources)
        pXRRFreeScreenResources( screen_resources );
    if (provider_info)
        pXRRFreeProviderInfo( provider_info );
    if (output_info)
        pXRRFreeOutputInfo( output_info );
    if (crtc_info)
        pXRRFreeCrtcInfo( crtc_info );
    if (!ret)
    {
        heap_free( adapters );
        ERR("Failed to get adapters\n");
    }
    return ret;
}

static void xrandr14_free_adapters( struct x11drv_adapter *adapters )
{
    heap_free( adapters );
}

static BOOL xrandr14_get_monitors( ULONG_PTR adapter_id, struct x11drv_monitor **new_monitors, int *count )
{
    static const WCHAR generic_nonpnp_monitorW[] = {
        'G','e','n','e','r','i','c',' ',
        'N','o','n','-','P','n','P',' ','M','o','n','i','t','o','r',0};
    struct x11drv_monitor *realloc_monitors, *monitors = NULL;
    XRRScreenResources *screen_resources = NULL;
    XRROutputInfo *output_info = NULL, *enum_output_info = NULL;
    XRRCrtcInfo *crtc_info = NULL, *enum_crtc_info;
    INT primary_index = -1, monitor_count = 0, capacity;
    RECT primary_rect;
    BOOL ret = FALSE;
    INT i;

    screen_resources = xrandr_get_screen_resources();
    if (!screen_resources)
        goto done;

    /* First start with a 2 monitors, should be enough for most cases */
    capacity = 2;
    monitors = heap_calloc( capacity, sizeof(*monitors) );
    if (!monitors)
        goto done;

    output_info = pXRRGetOutputInfo( gdi_display, screen_resources, adapter_id );
    if (!output_info)
        goto done;

    if (output_info->crtc)
    {
        crtc_info = pXRRGetCrtcInfo( gdi_display, screen_resources, output_info->crtc );
        if (!crtc_info)
            goto done;
    }

    /* Inactive but attached monitor, no need to check for mirrored/replica monitors */
    if (!output_info->crtc || !crtc_info->mode)
    {
        lstrcpyW( monitors[monitor_count].name, generic_nonpnp_monitorW );
        monitors[monitor_count].state_flags = DISPLAY_DEVICE_ATTACHED;
        monitor_count = 1;
    }
    /* Active monitors, need to find other monitors with the same coordinates as mirrored */
    else
    {
        primary_rect = get_primary_rect( screen_resources );

        for (i = 0; i < screen_resources->noutput; ++i)
        {
            enum_output_info = pXRRGetOutputInfo( gdi_display, screen_resources, screen_resources->outputs[i] );
            if (!enum_output_info)
                goto done;

            /* Detached outputs don't count */
            if (enum_output_info->connection != RR_Connected)
            {
                pXRRFreeOutputInfo( enum_output_info );
                enum_output_info = NULL;
                continue;
            }

            /* Allocate more space if needed */
            if (monitor_count >= capacity)
            {
                capacity *= 2;
                realloc_monitors = heap_realloc( monitors, capacity * sizeof(*monitors) );
                if (!realloc_monitors)
                    goto done;
                monitors = realloc_monitors;
            }

            if (enum_output_info->crtc)
            {
                enum_crtc_info = pXRRGetCrtcInfo( gdi_display, screen_resources, enum_output_info->crtc );
                if (!enum_crtc_info)
                    goto done;

                if (enum_crtc_info->x == crtc_info->x &&
                    enum_crtc_info->y == crtc_info->y &&
                    enum_crtc_info->width == crtc_info->width &&
                    enum_crtc_info->height == crtc_info->height)
                {
                    /* FIXME: Read output EDID property and parse the data to get the correct name */
                    lstrcpyW( monitors[monitor_count].name, generic_nonpnp_monitorW );

                    SetRect( &monitors[monitor_count].rc_monitor, crtc_info->x, crtc_info->y,
                             crtc_info->x + crtc_info->width, crtc_info->y + crtc_info->height );
                    monitors[monitor_count].rc_work = get_work_area( &monitors[monitor_count].rc_monitor );

                    monitors[monitor_count].state_flags = DISPLAY_DEVICE_ATTACHED;
                    if (!IsRectEmpty( &monitors[monitor_count].rc_monitor ))
                        monitors[monitor_count].state_flags |= DISPLAY_DEVICE_ACTIVE;

                    if (is_crtc_primary( primary_rect, crtc_info ))
                        primary_index = monitor_count;
                    monitor_count++;
                }

                pXRRFreeCrtcInfo( enum_crtc_info );
            }

            pXRRFreeOutputInfo( enum_output_info );
            enum_output_info = NULL;
        }

        /* Make sure the first monitor is the primary */
        if (primary_index > 0)
        {
            struct x11drv_monitor tmp = monitors[0];
            monitors[0] = monitors[primary_index];
            monitors[primary_index] = tmp;
        }

        /* Make sure the primary monitor origin is at (0, 0) */
        for (i = 0; i < monitor_count; i++)
        {
            OffsetRect( &monitors[i].rc_monitor, -primary_rect.left, -primary_rect.top );
            OffsetRect( &monitors[i].rc_work, -primary_rect.left, -primary_rect.top );
        }

        if (primary_index >= 0 && fs_hack_enabled())
        {
            /* apply fs hack to primary monitor */
            POINT fs_hack = fs_hack_current_mode();

            monitors[0].rc_monitor.right = monitors[0].rc_monitor.left + fs_hack.x;
            monitors[0].rc_monitor.bottom = monitors[0].rc_monitor.top + fs_hack.y;

            fs_hack.x = monitors[0].rc_work.left;
            fs_hack.y = monitors[0].rc_work.top;
            fs_hack_real_to_user(&fs_hack);
            monitors[0].rc_work.left = fs_hack.x;
            monitors[0].rc_work.top = fs_hack.y;

            fs_hack.x = monitors[0].rc_work.right;
            fs_hack.y = monitors[0].rc_work.bottom;
            fs_hack_real_to_user(&fs_hack);
            monitors[0].rc_work.right = fs_hack.x;
            monitors[0].rc_work.bottom = fs_hack.y;

            /* TODO adjust other monitor positions */
        }
    }

    *new_monitors = monitors;
    *count = monitor_count;
    ret = TRUE;
done:
    if (screen_resources)
        pXRRFreeScreenResources( screen_resources );
    if (output_info)
        pXRRFreeOutputInfo( output_info);
    if (crtc_info)
        pXRRFreeCrtcInfo( crtc_info );
    if (enum_output_info)
        pXRRFreeOutputInfo( enum_output_info );
    if (!ret)
    {
        heap_free( monitors );
        ERR("Failed to get monitors\n");
    }
    return ret;
}

static void xrandr14_free_monitors( struct x11drv_monitor *monitors )
{
    heap_free( monitors );
}

static BOOL xrandr14_device_change_handler( HWND hwnd, XEvent *event )
{
    if (hwnd == GetDesktopWindow() && GetWindowThreadProcessId( hwnd, NULL ) == GetCurrentThreadId())
    {
        /* Don't send a WM_DISPLAYCHANGE message here because this event may be a result from
         * ChangeDisplaySettings(). Otherwise, ChangeDisplaySettings() would send multiple
         * WM_DISPLAYCHANGE messages instead of just one */
        X11DRV_DisplayDevices_Update( FALSE );
    }
    return FALSE;
}

static void xrandr14_register_event_handlers(void)
{
    Display *display = thread_init_display();
    int event_base, error_base;

    if (!pXRRQueryExtension( display, &event_base, &error_base ))
        return;

    pXRRSelectInput( display, root_window,
                     RRCrtcChangeNotifyMask | RROutputChangeNotifyMask | RRProviderChangeNotifyMask );
    X11DRV_register_event_handler( event_base + RRNotify_CrtcChange, xrandr14_device_change_handler,
                                   "XRandR CrtcChange" );
    X11DRV_register_event_handler( event_base + RRNotify_OutputChange, xrandr14_device_change_handler,
                                   "XRandR OutputChange" );
    X11DRV_register_event_handler( event_base + RRNotify_ProviderChange, xrandr14_device_change_handler,
                                   "XRandR ProviderChange" );
}

/* XRandR 1.4 display settings handler */
static BOOL xrandr14_get_id( const WCHAR *device_name, ULONG_PTR *id )
{
    INT gpu_count, adapter_count, display_count = 0;
    INT gpu_idx, adapter_idx, display_idx;
    struct x11drv_adapter *adapters;
    struct x11drv_gpu *gpus;
    WCHAR *end;

    /* Parse \\.\DISPLAY%d */
    display_idx = strtolW( device_name + 11, &end, 10 ) - 1;
    if (*end)
        return FALSE;

    if (!xrandr14_get_gpus2( &gpus, &gpu_count, FALSE ))
        return FALSE;

    for (gpu_idx = 0; gpu_idx < gpu_count; ++gpu_idx)
    {
        if (!xrandr14_get_adapters( gpus[gpu_idx].id, &adapters, &adapter_count ))
        {
            xrandr14_free_gpus( gpus );
            return FALSE;
        }

        adapter_idx = display_idx - display_count;
        if (adapter_idx < adapter_count)
        {
            *id = adapters[adapter_idx].id;
            xrandr14_free_adapters( adapters );
            xrandr14_free_gpus( gpus );
            return TRUE;
        }

        display_count += adapter_count;
        xrandr14_free_adapters( adapters );
    }
    xrandr14_free_gpus( gpus );
    return FALSE;
}

static void add_xrandr14_mode( DEVMODEW *mode, XRRModeInfo *info, DWORD depth, DWORD frequency )
{
    mode->dmSize = sizeof(*mode);
    mode->dmDriverExtra = sizeof(RRMode);
    mode->dmFields = DM_DISPLAYORIENTATION | DM_BITSPERPEL | DM_PELSWIDTH |
                     DM_PELSHEIGHT | DM_DISPLAYFLAGS;
    if (frequency)
    {
        mode->dmFields |= DM_DISPLAYFREQUENCY;
        mode->dmDisplayFrequency = frequency;
    }
    mode->u1.s2.dmDisplayOrientation = DMDO_DEFAULT;
    mode->dmBitsPerPel = depth;
    mode->dmPelsWidth = info->width;
    mode->dmPelsHeight = info->height;
    mode->u2.dmDisplayFlags = 0;
    memcpy( (BYTE *)mode + sizeof(*mode), &info->id, sizeof(info->id) );
}

static BOOL xrandr14_get_modes( ULONG_PTR id, DWORD flags, DEVMODEW **new_modes, UINT *mode_count )
{
    XRRScreenResources *screen_resources;
    XRROutputInfo *output_info = NULL;
    RROutput output = (RROutput)id;
    UINT depth_idx, mode_idx = 0;
    XRRModeInfo *mode_info;
    DEVMODEW *mode, *modes;
    BOOL ret = FALSE;
    DWORD frequency;
    INT i, j;

    screen_resources = xrandr_get_screen_resources();
    if (!screen_resources)
        goto done;

    output_info = pXRRGetOutputInfo( gdi_display, screen_resources, output );
    if (!output_info)
        goto done;

    if (output_info->connection != RR_Connected)
    {
        ret = TRUE;
        *new_modes = NULL;
        *mode_count = 0;
        goto done;
    }

    /* Allocate space for display modes in different color depths.
     * Store a RRMode at the end of each DEVMODEW as private driver data */
    modes = heap_calloc( output_info->nmode * DEPTH_COUNT, sizeof(*modes) + sizeof(RRMode) );
    if (!modes)
        goto done;

    for (i = 0; i < output_info->nmode; ++i)
    {
        for (j = 0; j < screen_resources->nmode; ++j)
        {
            if (output_info->modes[i] != screen_resources->modes[j].id)
                continue;

            mode_info = &screen_resources->modes[j];
            frequency = get_frequency( mode_info );

            for (depth_idx = 0; depth_idx < DEPTH_COUNT; ++depth_idx)
            {
                mode = (DEVMODEW *)((BYTE *)modes + (sizeof(*modes) + sizeof(RRMode)) * mode_idx);
                add_xrandr14_mode( mode, mode_info, depths[depth_idx], frequency );
                ++mode_idx;
            }

            break;
        }
    }

    ret = TRUE;
    *new_modes = modes;
    *mode_count = mode_idx;
done:
    if (output_info)
        pXRRFreeOutputInfo( output_info );
    if (screen_resources)
        pXRRFreeScreenResources( screen_resources );
    return ret;
}

static void xrandr14_free_modes( DEVMODEW *modes )
{
    heap_free( modes );
}

static BOOL xrandr14_get_current_mode( ULONG_PTR id, DEVMODEW *mode )
{
    XRRScreenResources *screen_resources;
    XRROutputInfo *output_info = NULL;
    RROutput output = (RROutput)id;
    XRRModeInfo *mode_info = NULL;
    XRRCrtcInfo *crtc_info = NULL;
    BOOL ret = FALSE;
    RECT primary;
    INT mode_idx;

    screen_resources = xrandr_get_screen_resources();
    if (!screen_resources)
        goto done;

    output_info = pXRRGetOutputInfo( gdi_display, screen_resources, output );
    if (!output_info)
        goto done;

    if (output_info->crtc)
    {
        crtc_info = pXRRGetCrtcInfo( gdi_display, screen_resources, output_info->crtc );
        if (!crtc_info)
            goto done;
    }

    /* Detached */
    if (output_info->connection != RR_Connected || !output_info->crtc || !crtc_info->mode)
    {
        mode->dmFields = DM_DISPLAYORIENTATION | DM_BITSPERPEL | DM_PELSWIDTH | DM_PELSHEIGHT |
                         DM_DISPLAYFLAGS | DM_DISPLAYFREQUENCY | DM_POSITION;
        mode->u1.s2.dmDisplayOrientation = DMDO_DEFAULT;
        mode->dmBitsPerPel = 0;
        mode->dmPelsWidth = 0;
        mode->dmPelsHeight = 0;
        mode->u2.dmDisplayFlags = 0;
        mode->dmDisplayFrequency = 0;
        mode->u1.s2.dmPosition.x = 0;
        mode->u1.s2.dmPosition.y = 0;
        ret = TRUE;
        goto done;
    }

    /* Attached */
    for (mode_idx = 0; mode_idx < screen_resources->nmode; ++mode_idx)
    {
        if (crtc_info->mode == screen_resources->modes[mode_idx].id)
        {
            mode_info = &screen_resources->modes[mode_idx];
            break;
        }
    }

    if (!mode_info)
        goto done;

    mode->dmFields = DM_DISPLAYORIENTATION | DM_BITSPERPEL | DM_PELSWIDTH | DM_PELSHEIGHT |
                     DM_DISPLAYFLAGS | DM_DISPLAYFREQUENCY | DM_POSITION;
    mode->u1.s2.dmDisplayOrientation = DMDO_DEFAULT;
    mode->dmBitsPerPel = screen_bpp;
    mode->dmPelsWidth = mode_info->width;
    mode->dmPelsHeight = mode_info->height;
    mode->u2.dmDisplayFlags = 0;
    mode->dmDisplayFrequency = get_frequency( mode_info );
    /* Convert RandR coordinates to virtual screen coordinates */
    primary = get_primary_rect( screen_resources );
    mode->u1.s2.dmPosition.x = crtc_info->x - primary.left;
    mode->u1.s2.dmPosition.y = crtc_info->y - primary.top;
    ret = TRUE;
done:
    if (crtc_info)
        pXRRFreeCrtcInfo( crtc_info );
    if (output_info)
        pXRRFreeOutputInfo( output_info );
    if (screen_resources)
        pXRRFreeScreenResources( screen_resources );
    return ret;
}

static LONG xrandr14_set_current_mode( ULONG_PTR id, DEVMODEW *mode )
{
    unsigned int screen_width, screen_height;
    RROutput output = (RROutput)id, *outputs;
    XRRScreenResources *screen_resources;
    XRROutputInfo *output_info = NULL;
    XRRCrtcInfo *crtc_info = NULL;
    LONG ret = DISP_CHANGE_FAILED;
    INT crtc_idx, output_count;
    Rotation rotation;
    RRCrtc crtc = 0;
    Status status;
    RRMode rrmode;

    if (mode->dmFields & DM_BITSPERPEL && mode->dmBitsPerPel != screen_bpp)
        WARN("Cannot change screen color depth from %ubits to %ubits!\n", screen_bpp, mode->dmBitsPerPel);

    screen_resources = xrandr_get_screen_resources();
    if (!screen_resources)
        return ret;

    XGrabServer( gdi_display );

    output_info = pXRRGetOutputInfo( gdi_display, screen_resources, output );
    if (!output_info || output_info->connection != RR_Connected)
        goto done;

    /* Attached */
    if (output_info->crtc)
    {
        crtc = output_info->crtc;
    }
    /* Detached, need to find a free CRTC */
    else
    {
        for (crtc_idx = 0; crtc_idx < output_info->ncrtc; ++crtc_idx)
        {
            crtc_info = pXRRGetCrtcInfo( gdi_display, screen_resources, output_info->crtcs[crtc_idx] );
            if (!crtc_info)
                goto done;

            if (!crtc_info->noutput)
            {
                crtc = output_info->crtcs[crtc_idx];
                pXRRFreeCrtcInfo( crtc_info );
                crtc_info = NULL;
                break;
            }

            pXRRFreeCrtcInfo( crtc_info );
            crtc_info = NULL;
        }

        /* Failed to find a free CRTC */
        if (crtc_idx == output_info->ncrtc)
            goto done;
    }

    crtc_info = pXRRGetCrtcInfo( gdi_display, screen_resources, crtc );
    if (!crtc_info)
        goto done;

    assert( mode->dmDriverExtra == sizeof(RRMode) );
    memcpy( &rrmode, (BYTE *)mode + sizeof(*mode), sizeof(rrmode) );

    if (crtc_info->noutput)
    {
        outputs = crtc_info->outputs;
        output_count = crtc_info->noutput;
        rotation = crtc_info->rotation;
    }
    else
    {
        outputs = &output;
        output_count = 1;
        rotation = RR_Rotate_0;
    }

    /* According to the RandR spec, the entire CRTC must fit inside the screen.
     * Since we use the union of all enabled CRTCs to determine the necessary
     * screen size, this might involve shrinking the screen, so we must disable
     * the CRTC in question first. */
    status = pXRRSetCrtcConfig( gdi_display, screen_resources, crtc, CurrentTime, 0, 0, None,
                                RR_Rotate_0, NULL, 0 );
    if (status != RRSetConfigSuccess)
        goto done;

    get_screen_size( screen_resources, &screen_width, &screen_height );
    screen_width = max( screen_width, crtc_info->x + mode->dmPelsWidth );
    screen_height = max( screen_height, crtc_info->y + mode->dmPelsHeight );
    set_screen_size( screen_width, screen_height );

    status = pXRRSetCrtcConfig( gdi_display, screen_resources, crtc, CurrentTime,
                                crtc_info->x, crtc_info->y, rrmode, rotation, outputs, output_count );
    if (status == RRSetConfigSuccess)
        ret = DISP_CHANGE_SUCCESSFUL;

done:
    XUngrabServer( gdi_display );
    XFlush( gdi_display );
    if (crtc_info)
        pXRRFreeCrtcInfo( crtc_info );
    if (output_info)
        pXRRFreeOutputInfo( output_info );
    pXRRFreeScreenResources( screen_resources );
    return ret;
}

#endif

void X11DRV_XRandR_Init(void)
{
    struct x11drv_display_device_handler display_handler;
    struct x11drv_settings_handler settings_handler;
    int event_base, error_base, minor, ret;
    static int major;
    Bool ok;

    if (major) return; /* already initialized? */
    if (!usexrandr) return; /* disabled in config */
    if (is_virtual_desktop()) return;
    if (!(ret = load_xrandr())) return;  /* can't load the Xrandr library */

    /* see if Xrandr is available */
    if (!pXRRQueryExtension( gdi_display, &event_base, &error_base )) return;
    X11DRV_expect_error( gdi_display, XRandRErrorHandler, NULL );
    ok = pXRRQueryVersion( gdi_display, &major, &minor );
    if (X11DRV_check_error() || !ok) return;

    TRACE("Found XRandR %d.%d.\n", major, minor);

#ifdef HAVE_XRRGETSCREENRESOURCES
    if (ret >= 2 && (major > 1 || (major == 1 && minor >= 2)))
    {
        if (major > 1 || (major == 1 && minor >= 3))
            pXRRGetScreenResourcesCurrent = dlsym( xrandr_handle, "XRRGetScreenResourcesCurrent" );
        if (!pXRRGetScreenResourcesCurrent)
            pXRRGetScreenResourcesCurrent = pXRRGetScreenResources;
    }

    if (!pXRRGetScreenResourcesCurrent || xrandr12_init_modes() < 0)
#endif
        xrandr10_init_modes();

#ifdef HAVE_XRRGETPROVIDERRESOURCES
    if (ret >= 4 && (major > 1 || (major == 1 && minor >= 4)))
    {
        display_handler.name = "XRandR 1.4";
        display_handler.priority = 200;
        display_handler.get_gpus = xrandr14_get_gpus;
        display_handler.get_adapters = xrandr14_get_adapters;
        display_handler.get_monitors = xrandr14_get_monitors;
        display_handler.free_gpus = xrandr14_free_gpus;
        display_handler.free_adapters = xrandr14_free_adapters;
        display_handler.free_monitors = xrandr14_free_monitors;
        display_handler.register_event_handlers = xrandr14_register_event_handlers;
        X11DRV_DisplayDevices_SetHandler( &display_handler );

        settings_handler.name = "XRandR 1.4";
        settings_handler.priority = 300;
        settings_handler.get_id = xrandr14_get_id;
        settings_handler.get_modes = xrandr14_get_modes;
        settings_handler.free_modes = xrandr14_free_modes;
        settings_handler.get_current_mode = xrandr14_get_current_mode;
        settings_handler.set_current_mode = xrandr14_set_current_mode;
        X11DRV_Settings_SetHandler( &settings_handler );
    }
#endif
}

#else /* SONAME_LIBXRANDR */

void X11DRV_XRandR_Init(void)
{
    TRACE("XRandR support not compiled in.\n");
}

#endif /* SONAME_LIBXRANDR */
