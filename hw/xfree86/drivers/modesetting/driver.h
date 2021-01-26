/*
 * Copyright 2008 Tungsten Graphics, Inc., Cedar Park, Texas.
 * All Rights Reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sub license, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice (including the
 * next paragraph) shall be included in all copies or substantial portions
 * of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT.
 * IN NO EVENT SHALL TUNGSTEN GRAPHICS AND/OR ITS SUPPLIERS BE LIABLE FOR
 * ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 *
 * Author: Alan Hourihane <alanh@tungstengraphics.com>
 *
 */

#include <errno.h>
#include <drm.h>
#include <xf86drm.h>
#include <xf86xv.h>
#include <xf86Crtc.h>
#include <damage.h>
#include <X11/extensions/dpmsconst.h>

#ifdef GLAMOR_HAS_GBM
#define GLAMOR_FOR_XORG 1
#include "glamor.h"
#include <gbm.h>
#endif

#include "drmmode_display.h"
#define MS_LOGLEVEL_DEBUG 4

typedef enum {
    OPTION_SW_CURSOR,
    OPTION_DEVICE_PATH,
    OPTION_SHADOW_FB,
    OPTION_ACCEL_METHOD,
    OPTION_PAGEFLIP,
    OPTION_ZAPHOD_HEADS,
    OPTION_DOUBLE_SHADOW,
    OPTION_ATOMIC,
    OPTION_FLIP_FB,
    OPTION_NO_EDID,
} modesettingOpts;

typedef struct
{
    int fd;
    int fd_ref;
    unsigned long fd_wakeup_registered; /* server generation for which fd has been registered for wakeup handling */
    int fd_wakeup_ref;
    unsigned int assigned_crtcs;
} modesettingEntRec, *modesettingEntPtr;

typedef void (*ms_drm_handler_proc)(uint64_t frame,
                                    uint64_t usec,
                                    void *data);

typedef void (*ms_drm_abort_proc)(void *data);

/**
 * A tracked handler for an event that will hopefully be generated by
 * the kernel, and what to do when it is encountered.
 */
struct ms_drm_queue {
    struct xorg_list list;
    xf86CrtcPtr crtc;
    uint32_t seq;
    void *data;
    ScrnInfoPtr scrn;
    ms_drm_handler_proc handler;
    ms_drm_abort_proc abort;
};

typedef struct _modesettingRec {
    int fd;
    Bool fd_passed;

    int Chipset;
    EntityInfoPtr pEnt;

    Bool noAccel;
    CloseScreenProcPtr CloseScreen;
    CreateWindowProcPtr CreateWindow;
    unsigned int SaveGeneration;

    CreateScreenResourcesProcPtr createScreenResources;
    ScreenBlockHandlerProcPtr BlockHandler;
    miPointerSpriteFuncPtr SpriteFuncs;
    void *driver;

    drmmode_rec drmmode;

    drmEventContext event_context;

    /**
     * Page flipping stuff.
     *  @{
     */
    Bool atomic_modeset;
    Bool pending_modeset;
    /** @} */

    DamagePtr damage;
    Bool dirty_enabled;

    uint32_t cursor_width, cursor_height;

    Bool has_queue_sequence;
    Bool tried_queue_sequence;

    Bool kms_has_modifiers;

    XF86VideoAdaptorPtr adaptor;
} modesettingRec, *modesettingPtr;

#define modesettingPTR(p) ((modesettingPtr)((p)->driverPrivate))
modesettingEntPtr ms_ent_priv(ScrnInfoPtr scrn);

uint32_t ms_drm_queue_alloc(xf86CrtcPtr crtc,
                            void *data,
                            ms_drm_handler_proc handler,
                            ms_drm_abort_proc abort);

typedef enum ms_queue_flag {
    MS_QUEUE_ABSOLUTE = 0,
    MS_QUEUE_RELATIVE = 1,
    MS_QUEUE_NEXT_ON_MISS = 2
} ms_queue_flag;

Bool ms_queue_vblank(xf86CrtcPtr crtc, ms_queue_flag flags,
                     uint64_t msc, uint64_t *msc_queued, uint32_t seq);

void ms_drm_abort(ScrnInfoPtr scrn,
                  Bool (*match)(void *data, void *match_data),
                  void *match_data);
void ms_drm_abort_seq(ScrnInfoPtr scrn, uint32_t seq);

Bool ms_crtc_on(xf86CrtcPtr crtc);

xf86CrtcPtr ms_dri2_crtc_covering_drawable(DrawablePtr pDraw);
RRCrtcPtr   ms_randr_crtc_covering_drawable(DrawablePtr pDraw);

int ms_get_crtc_ust_msc(xf86CrtcPtr crtc, CARD64 *ust, CARD64 *msc);

uint64_t ms_kernel_msc_to_crtc_msc(xf86CrtcPtr crtc, uint64_t sequence, Bool is64bit);


Bool ms_dri2_screen_init(ScreenPtr screen);
void ms_dri2_close_screen(ScreenPtr screen);

Bool ms_vblank_screen_init(ScreenPtr screen);
void ms_vblank_close_screen(ScreenPtr screen);

Bool ms_present_screen_init(ScreenPtr screen);

typedef void (*ms_pageflip_handler_proc)(modesettingPtr ms,
                                         uint64_t frame,
                                         uint64_t usec,
                                         void *data);

typedef void (*ms_pageflip_abort_proc)(modesettingPtr ms, void *data);

Bool ms_do_pageflip_bo(ScreenPtr screen,
                       drmmode_bo *new_front_bo,
                       void *event,
                       int ref_crtc_vblank_pipe,
                       xf86CrtcPtr target_crtc,
                       Bool async,
                       ms_pageflip_handler_proc pageflip_handler,
                       ms_pageflip_abort_proc pageflip_abort);

Bool ms_do_pageflip(ScreenPtr screen,
                    PixmapPtr new_front,
                    void *event,
                    int ref_crtc_vblank_pipe,
                    Bool async,
                    ms_pageflip_handler_proc pageflip_handler,
                    ms_pageflip_abort_proc pageflip_abort);

int ms_flush_drm_events(ScreenPtr screen);

Bool ms_init_exa(ScrnInfoPtr scrn);
void ms_deinit_exa(ScrnInfoPtr scrn);
Bool ms_exa_set_pixmap_bo(ScrnInfoPtr scrn, PixmapPtr pPixmap,
                          struct dumb_bo *bo, Bool owned);
struct dumb_bo *ms_exa_bo_from_pixmap(ScreenPtr screen, PixmapPtr pixmap);
void ms_exa_exchange_buffers(PixmapPtr front, PixmapPtr back);
Bool ms_exa_back_pixmap_from_fd(PixmapPtr pixmap, int fd,
                                CARD16 width, CARD16 height,
                                CARD16 stride, CARD8 depth, CARD8 bpp);
int ms_exa_shareable_fd_from_pixmap(ScreenPtr screen, PixmapPtr pixmap,
                                    CARD16 *stride, CARD32 *size);

Bool ms_exa_prepare_access(PixmapPtr pPix, int index);
void ms_exa_finish_access(PixmapPtr pPix, int index);

Bool ms_exa_copy_area(PixmapPtr pSrc, PixmapPtr pDst,
                      pixman_f_transform_t *transform, RegionPtr clip);

XF86VideoAdaptorPtr ms_exa_xv_init(ScreenPtr screen, int num_texture_ports);

#ifdef DRI3
Bool ms_exa_dri3_init(ScreenPtr screen);
#endif

void ms_exchange_buffers(PixmapPtr front, PixmapPtr back);
int ms_name_from_pixmap(PixmapPtr pixmap, CARD16 *stride, CARD32 *size);
