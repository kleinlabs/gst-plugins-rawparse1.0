/*
 * GStreamer EGL/GLES Sink
 * Copyright (C) 2012 Collabora Ltd.
 *   @author: Reynaldo H. Verdejo Pinochet <reynaldo@collabora.com>
 *   @author: Sebastian Dröge <sebastian.droege@collabora.co.uk>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 *
 * Alternatively, the contents of this file may be used under the
 * GNU Lesser General Public License Version 2.1 (the "LGPL"), in
 * which case the following provisions apply instead of the ones
 * mentioned above:
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 02111-1307, USA.
 */

/**
 * SECTION:element-eglglessink
 *
 * EglGlesSink renders video frames on a EGL surface it sets up
 * from a window it either creates (on X11) or gets a handle to
 * through it's xOverlay interface. All the display/surface logic
 * in this sink uses EGL to interact with the native window system.
 * The rendering logic, in turn, uses OpenGL ES v2.
 *
 * This sink has been tested to work on X11/Mesa and on Android
 * (From Gingerbread on to Jelly Bean) and while it's currently
 * using an slow copy-over rendering path it has proven to be fast
 * enough on the devices we have tried it on. 
 *
 * <refsect2>
 * <title>Supported EGL/OpenGL ES versions</title>
 * <para>
 * This Sink uses EGLv1 and GLESv2
 * </para>
 * </refsect2>
 *
 * <refsect2>
 * <title>Example launch line</title>
 * |[
 * gst-launch -v -m videotestsrc ! eglglessink
 * ]|
 * </refsect2>
 *
 * <refsect2>
 * <title>Example launch line with internal window creation disabled</title>
 * <para>
 * By setting the can_create_window property to FALSE you can force the
 * sink to wait for a window handle through it's xOverlay interface even
 * if internal window creation is supported by the platform. Window creation
 * is only supported in X11 right now but it should be trivial to add support
 * for different platforms.
 * </para>
 * |[
 * gst-launch -v -m videotestsrc ! eglglessink can_create_window=FALSE
 * ]|
 * </refsect2>
 *
 * <refsect2>
 * <title>Scaling</title>
 * <para>
 * The sink will try it's best to consider the incoming frame's and display's
 * pixel aspect ratio and fill the corresponding surface without altering the
 * decoded frame's geometry when scaling. You can disable this logic by setting
 * the force_aspect_ratio property to FALSE, in which case the sink will just
 * fill the entire surface it has access to regardles of the PAR/DAR relationship.
 * </para>
 * <para>
 * Querying the display aspect ratio is only supported with EGL versions >= 1.2.
 * The sink will just assume the DAR to be 1/1 if it can't get access to this
 * information.
 * </para>
 * <para>
 * Here is an example launch line with the PAR/DAR aware scaling disabled:
 * </para>
 * |[
 * gst-launch -v -m videotestsrc ! eglglessink force_aspect_ratio=FALSE
 * ]|
 * </refsect2>
 */

#ifdef HAVE_CONFIG_H
#  include <config.h>
#endif

#include <string.h>
#include <gst/gst.h>
#include <gst/video/video.h>
#include <gst/video/gstvideosink.h>
#include <gst/interfaces/xoverlay.h>

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>

#include "video_platform_wrapper.h"

#include "gsteglglessink.h"

/* Some EGL implementations are reporting wrong
 * values for the display's EGL_PIXEL_ASPECT_RATIO.
 * They are required by the khronos specs to report
 * this value as w/h * EGL_DISPLAY_SCALING (Which is
 * a constant with value 10000) but at least the
 * Galaxy SIII (Android) is reporting just 1 when
 * w = h. We use these two to bound returned values to
 * sanity.
 */
#define EGL_SANE_DAR_MIN ((EGL_DISPLAY_SCALING)/10)
#define EGL_SANE_DAR_MAX ((EGL_DISPLAY_SCALING)*10)

GST_DEBUG_CATEGORY_STATIC (gst_eglglessink_debug);
#define GST_CAT_DEFAULT gst_eglglessink_debug

/* GLESv2 GLSL Shaders
 *
 * OpenGL ES Standard does not mandate YUV support. This is
 * why most of these shaders deal with Packed/Planar YUV->RGB
 * conversion.
 */

/* *INDENT-OFF* */
/* Direct vertex copy */
static const char *vert_COPY_prog = {
      "attribute vec3 position;"
      "attribute vec2 texpos;"
      "varying vec2 opos;"
      "void main(void)"
      "{"
      " opos = texpos;"
      " gl_Position = vec4(position, 1.0);"
      "}"
};

static const char *vert_COPY_prog_no_tex = {
      "attribute vec3 position;"
      "void main(void)"
      "{"
      " gl_Position = vec4(position, 1.0);"
      "}"
};

/* Paint all black */
static const char *frag_BLACK_prog = {
  "precision mediump float;"
      "void main(void)"
      "{"
      " gl_FragColor = vec4(0.0, 0.0, 0.0, 1.0);"
      "}"
};

/* Direct fragments copy */
static const char *frag_COPY_prog = {
  "precision mediump float;"
      "varying vec2 opos;"
      "uniform sampler2D tex;"
      "void main(void)"
      "{"
      " vec4 t = texture2D(tex, opos);"
      " gl_FragColor = vec4(t.rgb, 1.0);"
      "}"
};

/* Channel reordering for XYZ <-> ZYX conversion */
static const char *frag_REORDER_prog = {
  "precision mediump float;"
      "varying vec2 opos;"
      "uniform sampler2D tex;"
      "void main(void)"
      "{"
      " vec4 t = texture2D(tex, opos);"
      " gl_FragColor = vec4(t.%c, t.%c, t.%c, 1.0);"
      "}"
};

/* Packed YUV converters */

/** AYUV to RGB conversion */
static const char *frag_AYUV_prog = {
      "precision mediump float;"
      "varying vec2 opos;"
      "uniform sampler2D tex;"
      "const vec3 offset = vec3(-0.0625, -0.5, -0.5);"
      "const vec3 rcoeff = vec3(1.164, 0.000, 1.596);"
      "const vec3 gcoeff = vec3(1.164,-0.391,-0.813);"
      "const vec3 bcoeff = vec3(1.164, 2.018, 0.000);"
      "void main(void) {"
      "  float r,g,b;"
      "  vec3 yuv;"
      "  yuv  = texture2D(tex,opos).gba;"
      "  yuv += offset;"
      "  r = dot(yuv, rcoeff);"
      "  g = dot(yuv, gcoeff);"
      "  b = dot(yuv, bcoeff);"
      "  gl_FragColor=vec4(r,g,b,1.0);"
      "}"
};

/** YUY2/YVYU/UYVY to RGB conversion */
static const char *frag_YUY2_YVYU_UYVY_prog = {
      "precision mediump float;"
      "varying vec2 opos;"
      "uniform sampler2D Ytex, UVtex;"
      "const vec3 offset = vec3(-0.0625, -0.5, -0.5);"
      "const vec3 rcoeff = vec3(1.164, 0.000, 1.596);"
      "const vec3 gcoeff = vec3(1.164,-0.391,-0.813);"
      "const vec3 bcoeff = vec3(1.164, 2.018, 0.000);"
      "void main(void) {"
      "  float r, g, b;"
      "  vec3 yuv;"
      "  yuv.x = texture2D(Ytex,opos).%c;"
      "  yuv.yz = texture2D(UVtex,opos).%c%c;"
      "  yuv += offset;"
      "  r = dot(yuv, rcoeff);"
      "  g = dot(yuv, gcoeff);"
      "  b = dot(yuv, bcoeff);"
      "  gl_FragColor=vec4(r,g,b,1.0);"
      "}"
};

/* Planar YUV converters */

/** YUV to RGB conversion */
static const char *frag_PLANAR_YUV_prog = {
      "precision mediump float;"
      "varying vec2 opos;"
      "uniform sampler2D Ytex,Utex,Vtex;"
      "const vec3 offset = vec3(-0.0625, -0.5, -0.5);"
      "const vec3 rcoeff = vec3(1.164, 0.000, 1.596);"
      "const vec3 gcoeff = vec3(1.164,-0.391,-0.813);"
      "const vec3 bcoeff = vec3(1.164, 2.018, 0.000);"
      "void main(void) {"
      "  float r,g,b;"
      "  vec3 yuv;"
      "  yuv.x=texture2D(Ytex,opos).r;"
      "  yuv.y=texture2D(Utex,opos).r;"
      "  yuv.z=texture2D(Vtex,opos).r;"
      "  yuv += offset;"
      "  r = dot(yuv, rcoeff);"
      "  g = dot(yuv, gcoeff);"
      "  b = dot(yuv, bcoeff);"
      "  gl_FragColor=vec4(r,g,b,1.0);"
      "}"
};

/** NV12/NV21 to RGB conversion */
static const char *frag_NV12_NV21_prog = {
      "precision mediump float;"
      "varying vec2 opos;"
      "uniform sampler2D Ytex,UVtex;"
      "const vec3 offset = vec3(-0.0625, -0.5, -0.5);"
      "const vec3 rcoeff = vec3(1.164, 0.000, 1.596);"
      "const vec3 gcoeff = vec3(1.164,-0.391,-0.813);"
      "const vec3 bcoeff = vec3(1.164, 2.018, 0.000);"
      "void main(void) {"
      "  float r,g,b;"
      "  vec3 yuv;"
      "  yuv.x=texture2D(Ytex,opos).r;"
      "  yuv.yz=texture2D(UVtex,opos).%c%c;"
      "  yuv += offset;"
      "  r = dot(yuv, rcoeff);"
      "  g = dot(yuv, gcoeff);"
      "  b = dot(yuv, bcoeff);"
      "  gl_FragColor=vec4(r,g,b,1.0);"
      "}"
};
/* *INDENT-ON* */

/* Input capabilities. */
static GstStaticPadTemplate gst_eglglessink_sink_template_factory =
    GST_STATIC_PAD_TEMPLATE ("sink",
    GST_PAD_SINK,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS (GST_VIDEO_CAPS_RGBA ";" GST_VIDEO_CAPS_BGRA ";"
        GST_VIDEO_CAPS_ARGB ";" GST_VIDEO_CAPS_ABGR ";"
        GST_VIDEO_CAPS_RGBx ";" GST_VIDEO_CAPS_BGRx ";"
        GST_VIDEO_CAPS_xRGB ";" GST_VIDEO_CAPS_xBGR ";"
        GST_VIDEO_CAPS_YUV
        ("{ AYUV, Y444, I420, YV12, NV12, NV21, YUY2, YVYU, UYVY, Y42B, Y41B }")
        ";" GST_VIDEO_CAPS_RGB ";" GST_VIDEO_CAPS_BGR ";"
        GST_VIDEO_CAPS_RGB_16));

/* Filter signals and args */
enum
{
  /* FILL ME */
  LAST_SIGNAL
};

enum
{
  PROP_0,
  PROP_CREATE_WINDOW,
  PROP_FORCE_ASPECT_RATIO,
};

/* will probably move elsewhere */
static const EGLint eglglessink_RGBA8888_attribs[] = {
  EGL_RED_SIZE, 8,
  EGL_GREEN_SIZE, 8,
  EGL_BLUE_SIZE, 8,
  EGL_ALPHA_SIZE, 8,
  EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
  EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
  EGL_NONE
};

static const EGLint eglglessink_RGB888_attribs[] = {
  EGL_RED_SIZE, 8,
  EGL_GREEN_SIZE, 8,
  EGL_BLUE_SIZE, 8,
  EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
  EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
  EGL_NONE
};

static const EGLint eglglessink_RGB565_attribs[] = {
  EGL_RED_SIZE, 5,
  EGL_GREEN_SIZE, 6,
  EGL_BLUE_SIZE, 5,
  EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
  EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
  EGL_NONE
};

static void gst_eglglessink_finalize (GObject * object);
static void gst_eglglessink_get_property (GObject * object, guint prop_id,
    GValue * value, GParamSpec * pspec);
static void gst_eglglessink_set_property (GObject * object, guint prop_id,
    const GValue * value, GParamSpec * pspec);
static GstStateChangeReturn gst_eglglessink_change_state (GstElement * element,
    GstStateChange transition);
static GstFlowReturn gst_eglglessink_show_frame (GstVideoSink * vsink,
    GstBuffer * buf);
static gboolean gst_eglglessink_setcaps (GstBaseSink * bsink, GstCaps * caps);
static GstCaps *gst_eglglessink_getcaps (GstBaseSink * bsink);

/* XOverlay interface cruft */
static gboolean gst_eglglessink_interface_supported
    (GstImplementsInterface * iface, GType type);
static void gst_eglglessink_implements_init
    (GstImplementsInterfaceClass * klass);
static void gst_eglglessink_xoverlay_init (GstXOverlayClass * iface);
static void gst_eglglessink_init_interfaces (GType type);

/* Actual XOverlay interface funcs */
static void gst_eglglessink_expose (GstXOverlay * overlay);
static void gst_eglglessink_set_window_handle (GstXOverlay * overlay,
    guintptr id);
static void gst_eglglessink_set_render_rectangle (GstXOverlay * overlay, gint x,
    gint y, gint width, gint height);

/* Utility */
static GstEglGlesImageFmt *gst_eglglessink_get_compat_format_from_caps
    (GstEglGlesSink * eglglessink, GstCaps * caps);
static EGLNativeWindowType gst_eglglessink_create_window (GstEglGlesSink *
    eglglessink, gint width, gint height);
static inline gint
gst_eglglessink_fill_supported_fbuffer_configs (GstEglGlesSink * eglglessink);
static gboolean gst_eglglessink_init_egl_display (GstEglGlesSink * eglglessink);
static gboolean gst_eglglessink_choose_config (GstEglGlesSink * eglglessink);
static gboolean gst_eglglessink_init_egl_surface (GstEglGlesSink * eglglessink);
static void gst_eglglessink_init_egl_exts (GstEglGlesSink * eglglessink);
static gboolean gst_eglglessink_setup_vbo (GstEglGlesSink * eglglessink,
    gboolean reset);
static gboolean
gst_eglglessink_configure_caps (GstEglGlesSink * eglglessink, GstCaps * caps);
static GstFlowReturn gst_eglglessink_render_and_display (GstEglGlesSink * sink,
    GstBuffer * buf);
static GstFlowReturn gst_eglglessink_queue_buffer (GstEglGlesSink * sink,
    GstBuffer * buf);
static inline gboolean got_gl_error (const char *wtf);
static inline void show_egl_error (const char *wtf);
static void gst_eglglessink_wipe_fmt (gpointer data);
static inline gboolean egl_init (GstEglGlesSink * eglglessink);
static gboolean gst_eglglessink_context_make_current (GstEglGlesSink *
    eglglessink, gboolean bind);
static void gst_eglglessink_wipe_eglglesctx (GstEglGlesSink * eglglessink);
static inline void gst_eglglessink_reset_display_region (GstEglGlesSink *
    eglglessink);

GST_BOILERPLATE_FULL (GstEglGlesSink, gst_eglglessink, GstVideoSink,
    GST_TYPE_VIDEO_SINK, gst_eglglessink_init_interfaces);


static GstEglGlesImageFmt *
gst_eglglessink_get_compat_format_from_caps (GstEglGlesSink * eglglessink,
    GstCaps * caps)
{

  GList *list;
  GstEglGlesImageFmt *format;

  g_return_val_if_fail (GST_IS_EGLGLESSINK (eglglessink), 0);

  list = eglglessink->supported_fmts;

  /* Traverse the list trying to find a compatible format */
  while (list) {
    format = list->data;
    GST_DEBUG_OBJECT (eglglessink, "Checking compatibility between listed %"
        GST_PTR_FORMAT " and %" GST_PTR_FORMAT, format->caps, caps);
    if (format) {
      if (gst_caps_can_intersect (caps, format->caps)) {
        GST_INFO_OBJECT (eglglessink, "Found compatible format %d",
            format->fmt);
        GST_DEBUG_OBJECT (eglglessink,
            "Got caps %" GST_PTR_FORMAT " and this format can do %"
            GST_PTR_FORMAT, caps, format->caps);
        return format;
      }
    }
    list = g_list_next (list);
  }

  return NULL;
}

static inline gint
gst_eglglessink_fill_supported_fbuffer_configs (GstEglGlesSink * eglglessink)
{
  gint ret = 0;
  EGLint cfg_number;
  GstEglGlesImageFmt *format;
  GstCaps *caps;

  GST_DEBUG_OBJECT (eglglessink,
      "Building initial list of wanted eglattribs per format");

  /* Init supported format/caps list */
  caps = gst_caps_new_empty ();

  if (eglChooseConfig (eglglessink->eglglesctx.display,
          eglglessink_RGBA8888_attribs, NULL, 1, &cfg_number) != EGL_FALSE) {
    format = g_new0 (GstEglGlesImageFmt, 1);
    format->fmt = GST_EGLGLESSINK_IMAGE_RGBA8888;
    format->attribs = eglglessink_RGBA8888_attribs;
    format->caps = gst_video_format_new_template_caps (GST_VIDEO_FORMAT_RGBA);
    gst_caps_append (format->caps,
        gst_video_format_new_template_caps (GST_VIDEO_FORMAT_BGRA));
    gst_caps_append (format->caps,
        gst_video_format_new_template_caps (GST_VIDEO_FORMAT_ARGB));
    gst_caps_append (format->caps,
        gst_video_format_new_template_caps (GST_VIDEO_FORMAT_ABGR));
    gst_caps_append (format->caps,
        gst_video_format_new_template_caps (GST_VIDEO_FORMAT_RGBx));
    gst_caps_append (format->caps,
        gst_video_format_new_template_caps (GST_VIDEO_FORMAT_BGRx));
    gst_caps_append (format->caps,
        gst_video_format_new_template_caps (GST_VIDEO_FORMAT_xRGB));
    gst_caps_append (format->caps,
        gst_video_format_new_template_caps (GST_VIDEO_FORMAT_xBGR));
    gst_caps_append (format->caps,
        gst_video_format_new_template_caps (GST_VIDEO_FORMAT_AYUV));
    gst_caps_append (format->caps,
        gst_video_format_new_template_caps (GST_VIDEO_FORMAT_Y444));
    gst_caps_append (format->caps,
        gst_video_format_new_template_caps (GST_VIDEO_FORMAT_I420));
    gst_caps_append (format->caps,
        gst_video_format_new_template_caps (GST_VIDEO_FORMAT_YV12));
    gst_caps_append (format->caps,
        gst_video_format_new_template_caps (GST_VIDEO_FORMAT_NV12));
    gst_caps_append (format->caps,
        gst_video_format_new_template_caps (GST_VIDEO_FORMAT_NV21));
    gst_caps_append (format->caps,
        gst_video_format_new_template_caps (GST_VIDEO_FORMAT_YUY2));
    gst_caps_append (format->caps,
        gst_video_format_new_template_caps (GST_VIDEO_FORMAT_YVYU));
    gst_caps_append (format->caps,
        gst_video_format_new_template_caps (GST_VIDEO_FORMAT_UYVY));
    gst_caps_append (format->caps,
        gst_video_format_new_template_caps (GST_VIDEO_FORMAT_Y42B));
    gst_caps_append (format->caps,
        gst_video_format_new_template_caps (GST_VIDEO_FORMAT_Y41B));
    eglglessink->supported_fmts =
        g_list_append (eglglessink->supported_fmts, format);
    ret++;
    gst_caps_append (caps, gst_caps_ref (format->caps));
  } else {
    GST_INFO_OBJECT (eglglessink,
        "EGL display doesn't support RGBA8888 config");
  }

  if (eglChooseConfig (eglglessink->eglglesctx.display,
          eglglessink_RGB888_attribs, NULL, 1, &cfg_number) != EGL_FALSE) {
    format = g_new0 (GstEglGlesImageFmt, 1);
    format->fmt = GST_EGLGLESSINK_IMAGE_RGB888;
    format->attribs = eglglessink_RGB888_attribs;
    format->caps = gst_video_format_new_template_caps (GST_VIDEO_FORMAT_RGB);
    gst_caps_append (format->caps,
        gst_video_format_new_template_caps (GST_VIDEO_FORMAT_BGR));
    eglglessink->supported_fmts =
        g_list_append (eglglessink->supported_fmts, format);
    ret++;
    gst_caps_append (caps, gst_caps_ref (format->caps));
  } else {
    GST_INFO_OBJECT (eglglessink, "EGL display doesn't support RGB888 config");
  }

  if (eglChooseConfig (eglglessink->eglglesctx.display,
          eglglessink_RGB565_attribs, NULL, 1, &cfg_number) != EGL_FALSE) {
    format = g_new0 (GstEglGlesImageFmt, 1);
    format->fmt = GST_EGLGLESSINK_IMAGE_RGB565;
    format->attribs = eglglessink_RGB565_attribs;
    format->caps = gst_video_format_new_template_caps (GST_VIDEO_FORMAT_RGB16);
    eglglessink->supported_fmts =
        g_list_append (eglglessink->supported_fmts, format);
    ret++;
    gst_caps_append (caps, gst_caps_ref (format->caps));
  } else {
    GST_INFO_OBJECT (eglglessink, "EGL display doesn't support RGB565 config");
  }

  GST_OBJECT_LOCK (eglglessink);
  gst_caps_replace (&eglglessink->sinkcaps, caps);
  GST_OBJECT_UNLOCK (eglglessink);
  gst_caps_unref (caps);

  return ret;
}

static inline gboolean
egl_init (GstEglGlesSink * eglglessink)
{
  if (!platform_wrapper_init ()) {
    GST_ERROR_OBJECT (eglglessink, "Couldn't init EGL platform wrapper");
    goto HANDLE_ERROR;
  }

  if (!gst_eglglessink_init_egl_display (eglglessink)) {
    GST_ERROR_OBJECT (eglglessink, "Couldn't init EGL display");
    goto HANDLE_ERROR;
  }

  gst_eglglessink_init_egl_exts (eglglessink);

  if (!gst_eglglessink_fill_supported_fbuffer_configs (eglglessink)) {
    GST_ERROR_OBJECT (eglglessink, "Display support NONE of our configs");
    goto HANDLE_ERROR;
  }

  eglglessink->egl_started = TRUE;

  return TRUE;

HANDLE_ERROR:
  GST_ERROR_OBJECT (eglglessink, "Failed to perform EGL init");
  return FALSE;
}

static gpointer
render_thread_func (GstEglGlesSink * eglglessink)
{
  GstDataQueueItem *item = NULL;

  while (gst_data_queue_pop (eglglessink->queue, &item)) {
    GstBuffer *buf = NULL;

    GST_DEBUG_OBJECT (eglglessink, "Handling object %" GST_PTR_FORMAT,
        item->object);

    if (item->object) {
      GstCaps *caps;

      buf = GST_BUFFER (item->object);
      caps = GST_BUFFER_CAPS (buf);
      if (caps != eglglessink->configured_caps) {
        if (!gst_eglglessink_configure_caps (eglglessink, caps)) {
          eglglessink->last_flow = GST_FLOW_NOT_NEGOTIATED;
          g_mutex_lock (eglglessink->render_lock);
          g_cond_broadcast (eglglessink->render_cond);
          g_mutex_unlock (eglglessink->render_lock);
          item->destroy (item);
          break;
        }
      }
    }

    if (eglglessink->configured_caps) {
      eglglessink->last_flow =
          gst_eglglessink_render_and_display (eglglessink, buf);
    } else {
      GST_DEBUG_OBJECT (eglglessink, "No caps configured yet, not drawing anything");
    }

    if (buf) {
      g_mutex_lock (eglglessink->render_lock);
      g_cond_broadcast (eglglessink->render_cond);
      g_mutex_unlock (eglglessink->render_lock);
    }
    item->destroy (item);
    if (eglglessink->last_flow != GST_FLOW_OK)
      break;
    GST_DEBUG_OBJECT (eglglessink, "Successfully handled object");
  }

  if (eglglessink->last_flow == GST_FLOW_OK)
    eglglessink->last_flow = GST_FLOW_WRONG_STATE;

  GST_DEBUG_OBJECT (eglglessink, "Shutting down thread");

  /* EGL/GLES cleanup */
  gst_eglglessink_wipe_eglglesctx (eglglessink);

  if (eglglessink->configured_caps) {
    gst_caps_unref (eglglessink->configured_caps);
    eglglessink->configured_caps = NULL;
  }

  return NULL;
}

static void
gst_eglglessink_wipe_eglglesctx (GstEglGlesSink * eglglessink)
{
  glUseProgram (0);

  if (eglglessink->have_vbo) {
    glDeleteBuffers (1, &eglglessink->eglglesctx.position_buffer);
    glDeleteBuffers (1, &eglglessink->eglglesctx.index_buffer);
    eglglessink->have_vbo = FALSE;
  }

  if (eglglessink->have_texture) {
    glDeleteTextures (eglglessink->eglglesctx.n_textures,
        eglglessink->eglglesctx.texture);
    eglglessink->have_texture = FALSE;
    eglglessink->eglglesctx.n_textures = 0;
  }

  if (eglglessink->eglglesctx.glslprogram[0]) {
    glDetachShader (eglglessink->eglglesctx.glslprogram[0],
        eglglessink->eglglesctx.fragshader[0]);
    glDetachShader (eglglessink->eglglesctx.glslprogram[0],
        eglglessink->eglglesctx.vertshader[0]);
    glDeleteProgram (eglglessink->eglglesctx.glslprogram[0]);
    glDeleteShader (eglglessink->eglglesctx.fragshader[0]);
    glDeleteShader (eglglessink->eglglesctx.vertshader[0]);
    eglglessink->eglglesctx.glslprogram[0] = 0;
  }

  if (eglglessink->eglglesctx.glslprogram[1]) {
    glDetachShader (eglglessink->eglglesctx.glslprogram[1],
        eglglessink->eglglesctx.fragshader[1]);
    glDetachShader (eglglessink->eglglesctx.glslprogram[1],
        eglglessink->eglglesctx.vertshader[1]);
    glDeleteProgram (eglglessink->eglglesctx.glslprogram[1]);
    glDeleteShader (eglglessink->eglglesctx.fragshader[1]);
    glDeleteShader (eglglessink->eglglesctx.vertshader[1]);
    eglglessink->eglglesctx.glslprogram[1] = 0;
  }

  gst_eglglessink_context_make_current (eglglessink, FALSE);

  if (eglglessink->eglglesctx.surface) {
    eglDestroySurface (eglglessink->eglglesctx.display,
        eglglessink->eglglesctx.surface);
    eglglessink->eglglesctx.surface = NULL;
    eglglessink->have_surface = FALSE;
  }

  if (eglglessink->eglglesctx.eglcontext) {
    eglDestroyContext (eglglessink->eglglesctx.display,
        eglglessink->eglglesctx.eglcontext);
    eglglessink->eglglesctx.eglcontext = NULL;
  }

  gst_eglglessink_reset_display_region (eglglessink);
}

/* Reset display region
 * XXX: Should probably keep old ones if set_render_rect()
 * has been called.
 */
static inline void
gst_eglglessink_reset_display_region (GstEglGlesSink * eglglessink)
{
  GST_OBJECT_LOCK (eglglessink);
  eglglessink->display_region.w = 0;
  eglglessink->display_region.h = 0;
  GST_OBJECT_UNLOCK (eglglessink);
}

static gboolean
gst_eglglessink_start (GstEglGlesSink * eglglessink)
{
  GError *error = NULL;

  GST_DEBUG_OBJECT (eglglessink, "Starting");

  if (!eglglessink->egl_started) {
    GST_ERROR_OBJECT (eglglessink, "EGL uninitialized. Bailing out");
    goto HANDLE_ERROR;
  }

  /* Ask for a window to render to */
  if (!eglglessink->have_window)
    gst_x_overlay_prepare_xwindow_id (GST_X_OVERLAY (eglglessink));

  if (!eglglessink->have_window && !eglglessink->create_window) {
    GST_ERROR_OBJECT (eglglessink, "Window handle unavailable and we "
        "were instructed not to create an internal one. Bailing out.");
    goto HANDLE_ERROR;
  }

  gst_eglglessink_reset_display_region (eglglessink);
  eglglessink->last_flow = GST_FLOW_OK;
  gst_data_queue_set_flushing (eglglessink->queue, FALSE);

#if !GLIB_CHECK_VERSION (2, 31, 0)
  eglglessink->thread =
      g_thread_create ((GThreadFunc) render_thread_func, eglglessink, TRUE,
      &error);
#else
  eglglessink->thread = g_thread_try_new ("eglglessink-render",
      (GThreadFunc) render_thread_func, eglglessink, &error);
#endif

  if (!eglglessink->thread || error != NULL)
    goto HANDLE_ERROR;

  GST_DEBUG_OBJECT (eglglessink, "Started");

  return TRUE;

HANDLE_ERROR:
  GST_ERROR_OBJECT (eglglessink, "Couldn't start");
  g_clear_error (&error);
  return FALSE;
}

static gboolean
gst_eglglessink_stop (GstEglGlesSink * eglglessink)
{
  GST_DEBUG_OBJECT (eglglessink, "Stopping");

  gst_data_queue_set_flushing (eglglessink->queue, TRUE);
  g_mutex_lock (eglglessink->render_lock);
  g_cond_broadcast (eglglessink->render_cond);
  g_mutex_unlock (eglglessink->render_lock);

  if (eglglessink->thread) {
    g_thread_join (eglglessink->thread);
    eglglessink->thread = NULL;
  }
  eglglessink->last_flow = GST_FLOW_WRONG_STATE;

  if (eglglessink->using_own_window) {
    platform_destroy_native_window (eglglessink->eglglesctx.display,
        eglglessink->eglglesctx.used_window);
    eglglessink->eglglesctx.used_window = 0;
    eglglessink->have_window = FALSE;
  }
  eglglessink->eglglesctx.used_window = 0;
  if (eglglessink->current_caps) {
    gst_caps_unref (eglglessink->current_caps);
    eglglessink->current_caps = NULL;
  }

  GST_DEBUG_OBJECT (eglglessink, "Stopped");

  return TRUE;
}

static void
gst_eglglessink_xoverlay_init (GstXOverlayClass * iface)
{
  iface->set_window_handle = gst_eglglessink_set_window_handle;
  iface->expose = gst_eglglessink_expose;
  iface->set_render_rectangle = gst_eglglessink_set_render_rectangle;
}

static gboolean
gst_eglglessink_interface_supported (GstImplementsInterface * iface, GType type)
{
  return (type == GST_TYPE_X_OVERLAY);
}

static void
gst_eglglessink_implements_init (GstImplementsInterfaceClass * klass)
{
  klass->supported = gst_eglglessink_interface_supported;
}

static inline gboolean
got_gl_error (const char *wtf)
{
  GLuint error = GL_NO_ERROR;

  if ((error = glGetError ()) != GL_NO_ERROR) {
    GST_CAT_ERROR (GST_CAT_DEFAULT, "GL ERROR: %s returned %x", wtf, error);
    return TRUE;
  }
  return FALSE;
}

static inline void
show_egl_error (const char *wtf)
{
  EGLint error;

  if ((error = eglGetError ()) != EGL_SUCCESS)
    GST_CAT_DEBUG (GST_CAT_DEFAULT, "EGL ERROR: %s returned %x", wtf, error);
}

static EGLNativeWindowType
gst_eglglessink_create_window (GstEglGlesSink * eglglessink, gint width,
    gint height)
{
  EGLNativeWindowType window = 0;

  if (!eglglessink->create_window) {
    GST_ERROR_OBJECT (eglglessink, "This sink can't create a window by itself");
    return window;
  } else
    GST_INFO_OBJECT (eglglessink, "Attempting internal window creation");

  window = platform_create_native_window (width, height);
  if (!window) {
    GST_ERROR_OBJECT (eglglessink, "Could not create window");
    return window;
  }
  return window;
}

static void
gst_eglglessink_expose (GstXOverlay * overlay)
{
  GstEglGlesSink *eglglessink;
  GstFlowReturn ret;

  eglglessink = GST_EGLGLESSINK (overlay);
  GST_DEBUG_OBJECT (eglglessink, "Expose catched, redisplay");

  /* Render from last seen buffer */
  ret = gst_eglglessink_queue_buffer (eglglessink, NULL);
  if (ret == GST_FLOW_ERROR)
    GST_ERROR_OBJECT (eglglessink, "Redisplay failed");
}

/* Prints available EGL/GLES extensions 
 * If another rendering path is implemented this is the place
 * where you want to check for the availability of its supporting
 * EGL/GLES extensions.
 */
static void
gst_eglglessink_init_egl_exts (GstEglGlesSink * eglglessink)
{
  const char *eglexts;
  unsigned const char *glexts;

  eglexts = eglQueryString (eglglessink->eglglesctx.display, EGL_EXTENSIONS);
  glexts = glGetString (GL_EXTENSIONS);

  GST_DEBUG_OBJECT (eglglessink, "Available EGL extensions: %s\n",
      GST_STR_NULL (eglexts));
  GST_DEBUG_OBJECT (eglglessink, "Available GLES extensions: %s\n",
      GST_STR_NULL ((const char *) glexts));

  return;
}

static gboolean
gst_eglglessink_setup_vbo (GstEglGlesSink * eglglessink, gboolean reset)
{
  gdouble surface_width, surface_height;
  gdouble x1, x2, y1, y2;

  GST_INFO_OBJECT (eglglessink, "VBO setup. have_vbo:%d, should reset %d",
      eglglessink->have_vbo, reset);

  if (eglglessink->have_vbo && reset) {
    glDeleteBuffers (1, &eglglessink->eglglesctx.position_buffer);
    glDeleteBuffers (1, &eglglessink->eglglesctx.index_buffer);
    eglglessink->have_vbo = FALSE;
  }

  surface_width = eglglessink->eglglesctx.surface_width;
  surface_height = eglglessink->eglglesctx.surface_height;

  GST_DEBUG_OBJECT (eglglessink, "Performing VBO setup");

  x1 = (eglglessink->display_region.x / surface_width) * 2.0 - 1;
  y1 = (eglglessink->display_region.y / surface_height) * 2.0 - 1;
  x2 = ((eglglessink->display_region.x +
          eglglessink->display_region.w) / surface_width) * 2.0 - 1;
  y2 = ((eglglessink->display_region.y +
          eglglessink->display_region.h) / surface_height) * 2.0 - 1;

  eglglessink->eglglesctx.position_array[0].x = x2;
  eglglessink->eglglesctx.position_array[0].y = y2;
  eglglessink->eglglesctx.position_array[0].z = 0;
  eglglessink->eglglesctx.position_array[0].a = 1;
  eglglessink->eglglesctx.position_array[0].b = 0;

  eglglessink->eglglesctx.position_array[1].x = x2;
  eglglessink->eglglesctx.position_array[1].y = y1;
  eglglessink->eglglesctx.position_array[1].z = 0;
  eglglessink->eglglesctx.position_array[1].a = 1;
  eglglessink->eglglesctx.position_array[1].b = 1;

  eglglessink->eglglesctx.position_array[2].x = x1;
  eglglessink->eglglesctx.position_array[2].y = y2;
  eglglessink->eglglesctx.position_array[2].z = 0;
  eglglessink->eglglesctx.position_array[2].a = 0;
  eglglessink->eglglesctx.position_array[2].b = 0;

  eglglessink->eglglesctx.position_array[3].x = x1;
  eglglessink->eglglesctx.position_array[3].y = y1;
  eglglessink->eglglesctx.position_array[3].z = 0;
  eglglessink->eglglesctx.position_array[3].a = 0;
  eglglessink->eglglesctx.position_array[3].b = 1;

  if (eglglessink->display_region.x == 0) {
    /* Borders top/bottom */

    eglglessink->eglglesctx.position_array[4 + 0].x = 1;
    eglglessink->eglglesctx.position_array[4 + 0].y = 1;
    eglglessink->eglglesctx.position_array[4 + 0].z = 0;

    eglglessink->eglglesctx.position_array[4 + 1].x = x2;
    eglglessink->eglglesctx.position_array[4 + 1].y = y2;
    eglglessink->eglglesctx.position_array[4 + 1].z = 0;

    eglglessink->eglglesctx.position_array[4 + 2].x = -1;
    eglglessink->eglglesctx.position_array[4 + 2].y = 1;
    eglglessink->eglglesctx.position_array[4 + 2].z = 0;

    eglglessink->eglglesctx.position_array[4 + 3].x = x1;
    eglglessink->eglglesctx.position_array[4 + 3].y = y2;
    eglglessink->eglglesctx.position_array[4 + 3].z = 0;

    eglglessink->eglglesctx.position_array[8 + 0].x = 1;
    eglglessink->eglglesctx.position_array[8 + 0].y = y1;
    eglglessink->eglglesctx.position_array[8 + 0].z = 0;

    eglglessink->eglglesctx.position_array[8 + 1].x = 1;
    eglglessink->eglglesctx.position_array[8 + 1].y = -1;
    eglglessink->eglglesctx.position_array[8 + 1].z = 0;

    eglglessink->eglglesctx.position_array[8 + 2].x = x1;
    eglglessink->eglglesctx.position_array[8 + 2].y = y1;
    eglglessink->eglglesctx.position_array[8 + 2].z = 0;

    eglglessink->eglglesctx.position_array[8 + 3].x = -1;
    eglglessink->eglglesctx.position_array[8 + 3].y = -1;
    eglglessink->eglglesctx.position_array[8 + 3].z = 0;
  } else {
    /* Borders left/right */

    eglglessink->eglglesctx.position_array[4 + 0].x = x1;
    eglglessink->eglglesctx.position_array[4 + 0].y = 1;
    eglglessink->eglglesctx.position_array[4 + 0].z = 0;

    eglglessink->eglglesctx.position_array[4 + 1].x = x1;
    eglglessink->eglglesctx.position_array[4 + 1].y = -1;
    eglglessink->eglglesctx.position_array[4 + 1].z = 0;

    eglglessink->eglglesctx.position_array[4 + 2].x = -1;
    eglglessink->eglglesctx.position_array[4 + 2].y = 1;
    eglglessink->eglglesctx.position_array[4 + 2].z = 0;

    eglglessink->eglglesctx.position_array[4 + 3].x = -1;
    eglglessink->eglglesctx.position_array[4 + 3].y = -1;
    eglglessink->eglglesctx.position_array[4 + 3].z = 0;

    eglglessink->eglglesctx.position_array[8 + 0].x = 1;
    eglglessink->eglglesctx.position_array[8 + 0].y = 1;
    eglglessink->eglglesctx.position_array[8 + 0].z = 0;

    eglglessink->eglglesctx.position_array[8 + 1].x = 1;
    eglglessink->eglglesctx.position_array[8 + 1].y = -1;
    eglglessink->eglglesctx.position_array[8 + 1].z = 0;

    eglglessink->eglglesctx.position_array[8 + 2].x = x2;
    eglglessink->eglglesctx.position_array[8 + 2].y = y2;
    eglglessink->eglglesctx.position_array[8 + 2].z = 0;

    eglglessink->eglglesctx.position_array[8 + 3].x = x2;
    eglglessink->eglglesctx.position_array[8 + 3].y = -1;
    eglglessink->eglglesctx.position_array[8 + 3].z = 0;
  }

  eglglessink->eglglesctx.index_array[0] = 0;
  eglglessink->eglglesctx.index_array[1] = 1;
  eglglessink->eglglesctx.index_array[2] = 2;
  eglglessink->eglglesctx.index_array[3] = 3;

  glGenBuffers (1, &eglglessink->eglglesctx.position_buffer);
  glGenBuffers (1, &eglglessink->eglglesctx.index_buffer);
  if (got_gl_error ("glGenBuffers"))
    goto HANDLE_ERROR_LOCKED;

  glBindBuffer (GL_ARRAY_BUFFER, eglglessink->eglglesctx.position_buffer);
  if (got_gl_error ("glBindBuffer position_buffer"))
    goto HANDLE_ERROR_LOCKED;

  glBufferData (GL_ARRAY_BUFFER,
      sizeof (eglglessink->eglglesctx.position_array),
      eglglessink->eglglesctx.position_array, GL_STATIC_DRAW);
  if (got_gl_error ("glBufferData position_buffer"))
    goto HANDLE_ERROR_LOCKED;

  glBindBuffer (GL_ELEMENT_ARRAY_BUFFER, eglglessink->eglglesctx.index_buffer);
  if (got_gl_error ("glBindBuffer index_buffer"))
    goto HANDLE_ERROR_LOCKED;

  glBufferData (GL_ELEMENT_ARRAY_BUFFER,
      sizeof (eglglessink->eglglesctx.index_array),
      eglglessink->eglglesctx.index_array, GL_STATIC_DRAW);
  if (got_gl_error ("glBufferData index_buffer"))
    goto HANDLE_ERROR_LOCKED;

  eglglessink->have_vbo = TRUE;
  GST_DEBUG_OBJECT (eglglessink, "VBO setup done");

  return TRUE;

HANDLE_ERROR_LOCKED:
  GST_ERROR_OBJECT (eglglessink, "Unable to perform VBO setup");
  return FALSE;
}

/* XXX: Lock eglgles context? */
static gboolean
gst_eglglessink_update_surface_dimensions (GstEglGlesSink * eglglessink)
{
  gint width, height;

  /* Save surface dims */
  eglQuerySurface (eglglessink->eglglesctx.display,
      eglglessink->eglglesctx.surface, EGL_WIDTH, &width);
  eglQuerySurface (eglglessink->eglglesctx.display,
      eglglessink->eglglesctx.surface, EGL_HEIGHT, &height);

  if (width != eglglessink->eglglesctx.surface_width ||
      height != eglglessink->eglglesctx.surface_height) {
    eglglessink->eglglesctx.surface_width = width;
    eglglessink->eglglesctx.surface_height = height;
    GST_INFO_OBJECT (eglglessink, "Got surface of %dx%d pixels", width, height);
    return TRUE;
  }

  return FALSE;
}

static gboolean
gst_eglglessink_context_make_current (GstEglGlesSink * eglglessink,
    gboolean bind)
{
  g_assert (eglglessink->eglglesctx.display != NULL);

  if (bind && eglglessink->eglglesctx.surface &&
      eglglessink->eglglesctx.eglcontext) {
    EGLContext *ctx = eglGetCurrentContext ();

    if (ctx == eglglessink->eglglesctx.eglcontext) {
      GST_DEBUG_OBJECT (eglglessink,
          "Already attached the context to thread %p", g_thread_self ());
      return TRUE;
    }

    GST_DEBUG_OBJECT (eglglessink, "Attaching context to thread %p",
        g_thread_self ());
    if (!eglMakeCurrent (eglglessink->eglglesctx.display,
            eglglessink->eglglesctx.surface, eglglessink->eglglesctx.surface,
            eglglessink->eglglesctx.eglcontext)) {
      show_egl_error ("eglMakeCurrent");
      GST_ERROR_OBJECT (eglglessink, "Couldn't bind context");
      return FALSE;
    }
  } else {
    GST_DEBUG_OBJECT (eglglessink, "Detaching context from thread %p",
        g_thread_self ());
    if (!eglMakeCurrent (eglglessink->eglglesctx.display, EGL_NO_SURFACE,
            EGL_NO_SURFACE, EGL_NO_CONTEXT)) {
      show_egl_error ("eglMakeCurrent");
      GST_ERROR_OBJECT (eglglessink, "Couldn't unbind context");
      return FALSE;
    }
  }

  return TRUE;
}

static gboolean
gst_eglglessink_init_egl_surface (GstEglGlesSink * eglglessink)
{
  GLint test;
  GLboolean ret;
  GLchar *info_log;
  EGLint display_par;
  const gchar *texnames[3] = { NULL, };
  gchar *tmp_prog = NULL;
  EGLint swap_behavior;

  GST_DEBUG_OBJECT (eglglessink, "Enter EGL surface setup");

  eglglessink->eglglesctx.surface =
      eglCreateWindowSurface (eglglessink->eglglesctx.display,
      eglglessink->eglglesctx.config, eglglessink->eglglesctx.used_window,
      NULL);

  if (eglglessink->eglglesctx.surface == EGL_NO_SURFACE) {
    show_egl_error ("eglCreateWindowSurface");
    GST_ERROR_OBJECT (eglglessink, "Can't create surface");
    goto HANDLE_EGL_ERROR_LOCKED;
  }

  eglglessink->eglglesctx.buffer_preserved = FALSE;
  if (eglQuerySurface (eglglessink->eglglesctx.display,
          eglglessink->eglglesctx.surface, EGL_SWAP_BEHAVIOR, &swap_behavior)) {
    GST_DEBUG_OBJECT (eglglessink, "Buffer swap behavior %x", swap_behavior);
    eglglessink->eglglesctx.buffer_preserved =
        swap_behavior == EGL_BUFFER_PRESERVED;
  } else {
    GST_DEBUG_OBJECT (eglglessink, "Can't query buffer swap behavior");
  }

  if (!gst_eglglessink_context_make_current (eglglessink, TRUE))
    goto HANDLE_EGL_ERROR_LOCKED;

  /* Save display's pixel aspect ratio
   *
   * DAR is reported as w/h * EGL_DISPLAY_SCALING wich is
   * a constant with value 10000. This attribute is only
   * supported if the EGL version is >= 1.2
   * XXX: Setup this as a property.
   * or some other one time check. Right now it's being called once
   * per frame.
   */
  if (eglglessink->eglglesctx.egl_major == 1 &&
      eglglessink->eglglesctx.egl_minor < 2) {
    GST_DEBUG_OBJECT (eglglessink, "Can't query PAR. Using default: %dx%d",
        EGL_DISPLAY_SCALING, EGL_DISPLAY_SCALING);
    eglglessink->eglglesctx.pixel_aspect_ratio = EGL_DISPLAY_SCALING;
  } else {
    eglQuerySurface (eglglessink->eglglesctx.display,
        eglglessink->eglglesctx.surface, EGL_PIXEL_ASPECT_RATIO, &display_par);
    /* Fix for outbound DAR reporting on some implementations not
     * honoring the 'should return w/h * EGL_DISPLAY_SCALING' spec
     * requirement
     */
    if (display_par == EGL_UNKNOWN || display_par < EGL_SANE_DAR_MIN ||
        display_par > EGL_SANE_DAR_MAX) {
      GST_DEBUG_OBJECT (eglglessink, "Nonsensical PAR value returned: %d. "
          "Bad EGL implementation? "
          "Will use default: %d/%d", eglglessink->eglglesctx.pixel_aspect_ratio,
          EGL_DISPLAY_SCALING, EGL_DISPLAY_SCALING);
      eglglessink->eglglesctx.pixel_aspect_ratio = EGL_DISPLAY_SCALING;
    } else {
      eglglessink->eglglesctx.pixel_aspect_ratio = display_par;
    }
  }

  /* Save surface dims */
  gst_eglglessink_update_surface_dimensions (eglglessink);

  /* We have a surface! */
  eglglessink->have_surface = TRUE;

  /* Init vertex and fragment GLSL shaders. 
   * Note: Shader compiler support is optional but we currently rely on it.
   */

  glGetBooleanv (GL_SHADER_COMPILER, &ret);
  if (ret == GL_FALSE) {
    GST_ERROR_OBJECT (eglglessink, "Shader compiler support is unavailable!");
    goto HANDLE_ERROR;
  }

  /* Build shader program for video texture rendering */
  eglglessink->eglglesctx.vertshader[0] = glCreateShader (GL_VERTEX_SHADER);
  GST_DEBUG_OBJECT (eglglessink, "Sending %s to handle %d", vert_COPY_prog,
      eglglessink->eglglesctx.vertshader[0]);
  glShaderSource (eglglessink->eglglesctx.vertshader[0], 1, &vert_COPY_prog,
      NULL);
  if (got_gl_error ("glShaderSource vertex"))
    goto HANDLE_ERROR;

  glCompileShader (eglglessink->eglglesctx.vertshader[0]);
  if (got_gl_error ("glCompileShader vertex"))
    goto HANDLE_ERROR;

  glGetShaderiv (eglglessink->eglglesctx.vertshader[0], GL_COMPILE_STATUS,
      &test);
  if (test != GL_FALSE)
    GST_DEBUG_OBJECT (eglglessink, "Successfully compiled vertex shader");
  else {
    GST_ERROR_OBJECT (eglglessink, "Couldn't compile vertex shader");
    glGetShaderiv (eglglessink->eglglesctx.vertshader[0], GL_INFO_LOG_LENGTH,
        &test);
    info_log = g_new0 (GLchar, test);
    glGetShaderInfoLog (eglglessink->eglglesctx.vertshader[0], test, NULL,
        info_log);
    GST_INFO_OBJECT (eglglessink, "Compilation info log:\n%s", info_log);
    g_free (info_log);
    goto HANDLE_ERROR;
  }

  eglglessink->eglglesctx.fragshader[0] = glCreateShader (GL_FRAGMENT_SHADER);
  switch (eglglessink->format) {
    case GST_VIDEO_FORMAT_AYUV:
      glShaderSource (eglglessink->eglglesctx.fragshader[0], 1, &frag_AYUV_prog,
          NULL);
      eglglessink->eglglesctx.n_textures = 1;
      texnames[0] = "tex";
      break;
    case GST_VIDEO_FORMAT_Y444:
    case GST_VIDEO_FORMAT_I420:
    case GST_VIDEO_FORMAT_YV12:
    case GST_VIDEO_FORMAT_Y42B:
    case GST_VIDEO_FORMAT_Y41B:
      glShaderSource (eglglessink->eglglesctx.fragshader[0], 1,
          &frag_PLANAR_YUV_prog, NULL);
      eglglessink->eglglesctx.n_textures = 3;
      texnames[0] = "Ytex";
      texnames[1] = "Utex";
      texnames[2] = "Vtex";
      break;
    case GST_VIDEO_FORMAT_YUY2:
      tmp_prog = g_strdup_printf (frag_YUY2_YVYU_UYVY_prog, 'r', 'g', 'a');
      glShaderSource (eglglessink->eglglesctx.fragshader[0], 1,
          (const GLchar **) &tmp_prog, NULL);
      eglglessink->eglglesctx.n_textures = 2;
      texnames[0] = "Ytex";
      texnames[1] = "UVtex";
      break;
    case GST_VIDEO_FORMAT_YVYU:
      tmp_prog = g_strdup_printf (frag_YUY2_YVYU_UYVY_prog, 'r', 'a', 'g');
      glShaderSource (eglglessink->eglglesctx.fragshader[0], 1,
          (const GLchar **) &tmp_prog, NULL);
      eglglessink->eglglesctx.n_textures = 2;
      texnames[0] = "Ytex";
      texnames[1] = "UVtex";
      break;
    case GST_VIDEO_FORMAT_UYVY:
      tmp_prog = g_strdup_printf (frag_YUY2_YVYU_UYVY_prog, 'a', 'r', 'b');
      glShaderSource (eglglessink->eglglesctx.fragshader[0], 1,
          (const GLchar **) &tmp_prog, NULL);
      eglglessink->eglglesctx.n_textures = 2;
      texnames[0] = "Ytex";
      texnames[1] = "UVtex";
      break;
    case GST_VIDEO_FORMAT_NV12:
      tmp_prog = g_strdup_printf (frag_NV12_NV21_prog, 'r', 'a');
      glShaderSource (eglglessink->eglglesctx.fragshader[0], 1,
          (const GLchar **) &tmp_prog, NULL);
      eglglessink->eglglesctx.n_textures = 2;
      texnames[0] = "Ytex";
      texnames[1] = "UVtex";
      break;
    case GST_VIDEO_FORMAT_NV21:
      tmp_prog = g_strdup_printf (frag_NV12_NV21_prog, 'a', 'r');
      glShaderSource (eglglessink->eglglesctx.fragshader[0], 1,
          (const GLchar **) &tmp_prog, NULL);
      eglglessink->eglglesctx.n_textures = 2;
      texnames[0] = "Ytex";
      texnames[1] = "UVtex";
      break;
    case GST_VIDEO_FORMAT_BGR:
    case GST_VIDEO_FORMAT_BGRx:
    case GST_VIDEO_FORMAT_BGRA:
      tmp_prog = g_strdup_printf (frag_REORDER_prog, 'b', 'g', 'r');
      glShaderSource (eglglessink->eglglesctx.fragshader[0], 1,
          (const GLchar **) &tmp_prog, NULL);
      eglglessink->eglglesctx.n_textures = 1;
      texnames[0] = "tex";
      break;
    case GST_VIDEO_FORMAT_xRGB:
    case GST_VIDEO_FORMAT_ARGB:
      tmp_prog = g_strdup_printf (frag_REORDER_prog, 'g', 'b', 'a');
      glShaderSource (eglglessink->eglglesctx.fragshader[0], 1,
          (const GLchar **) &tmp_prog, NULL);
      eglglessink->eglglesctx.n_textures = 1;
      texnames[0] = "tex";
      break;
    case GST_VIDEO_FORMAT_xBGR:
    case GST_VIDEO_FORMAT_ABGR:
      tmp_prog = g_strdup_printf (frag_REORDER_prog, 'a', 'b', 'g');
      glShaderSource (eglglessink->eglglesctx.fragshader[0], 1,
          (const GLchar **) &tmp_prog, NULL);
      eglglessink->eglglesctx.n_textures = 1;
      texnames[0] = "tex";
      break;
    case GST_VIDEO_FORMAT_RGB:
    case GST_VIDEO_FORMAT_RGBx:
    case GST_VIDEO_FORMAT_RGBA:
    case GST_VIDEO_FORMAT_RGB16:
      glShaderSource (eglglessink->eglglesctx.fragshader[0], 1, &frag_COPY_prog,
          NULL);
      eglglessink->eglglesctx.n_textures = 1;
      texnames[0] = "tex";
      break;
    default:
      g_assert_not_reached ();
      break;
  }

  if (got_gl_error ("glShaderSource fragment"))
    goto HANDLE_ERROR;

  glCompileShader (eglglessink->eglglesctx.fragshader[0]);
  if (got_gl_error ("glCompileShader fragment"))
    goto HANDLE_ERROR;

  glGetShaderiv (eglglessink->eglglesctx.fragshader[0], GL_COMPILE_STATUS,
      &test);
  if (test != GL_FALSE)
    GST_DEBUG_OBJECT (eglglessink, "Successfully compiled fragment shader");
  else {
    GST_ERROR_OBJECT (eglglessink, "Couldn't compile fragment shader");
    glGetShaderiv (eglglessink->eglglesctx.fragshader[0], GL_INFO_LOG_LENGTH,
        &test);
    info_log = g_new0 (GLchar, test);
    glGetShaderInfoLog (eglglessink->eglglesctx.fragshader[0], test, NULL,
        info_log);
    GST_INFO_OBJECT (eglglessink, "Compilation info log:\n%s", info_log);
    g_free (info_log);
    goto HANDLE_ERROR;
  }

  eglglessink->eglglesctx.glslprogram[0] = glCreateProgram ();
  if (got_gl_error ("glCreateProgram"))
    goto HANDLE_ERROR;
  glAttachShader (eglglessink->eglglesctx.glslprogram[0],
      eglglessink->eglglesctx.vertshader[0]);
  if (got_gl_error ("glAttachShader vertices"))
    goto HANDLE_ERROR;
  glAttachShader (eglglessink->eglglesctx.glslprogram[0],
      eglglessink->eglglesctx.fragshader[0]);
  if (got_gl_error ("glAttachShader fragments"))
    goto HANDLE_ERROR;
  glLinkProgram (eglglessink->eglglesctx.glslprogram[0]);
  glGetProgramiv (eglglessink->eglglesctx.glslprogram[0], GL_LINK_STATUS,
      &test);
  if (test != GL_FALSE)
    GST_DEBUG_OBJECT (eglglessink, "GLES: Successfully linked program");
  else {
    GST_ERROR_OBJECT (eglglessink, "Couldn't link program");
    goto HANDLE_ERROR;
  }

  eglglessink->eglglesctx.position_loc[0] =
      glGetAttribLocation (eglglessink->eglglesctx.glslprogram[0], "position");
  eglglessink->eglglesctx.texpos_loc =
      glGetAttribLocation (eglglessink->eglglesctx.glslprogram[0], "texpos");

  glEnableVertexAttribArray (eglglessink->eglglesctx.position_loc[0]);
  if (got_gl_error ("glEnableVertexAttribArray"))
    goto HANDLE_ERROR;

  glEnableVertexAttribArray (eglglessink->eglglesctx.texpos_loc);
  if (got_gl_error ("glEnableVertexAttribArray"))
    goto HANDLE_ERROR;

  if (!eglglessink->eglglesctx.buffer_preserved) {
    /* Build shader program for black borders */
    eglglessink->eglglesctx.vertshader[1] = glCreateShader (GL_VERTEX_SHADER);
    GST_DEBUG_OBJECT (eglglessink, "Sending %s to handle %d",
        vert_COPY_prog_no_tex, eglglessink->eglglesctx.vertshader[1]);
    glShaderSource (eglglessink->eglglesctx.vertshader[1], 1,
        &vert_COPY_prog_no_tex, NULL);
    if (got_gl_error ("glShaderSource vertex"))
      goto HANDLE_ERROR;

    glCompileShader (eglglessink->eglglesctx.vertshader[1]);
    if (got_gl_error ("glCompileShader vertex"))
      goto HANDLE_ERROR;

    glGetShaderiv (eglglessink->eglglesctx.vertshader[1], GL_COMPILE_STATUS,
        &test);
    if (test != GL_FALSE)
      GST_DEBUG_OBJECT (eglglessink, "Successfully compiled vertex shader");
    else {
      GST_ERROR_OBJECT (eglglessink, "Couldn't compile vertex shader");
      glGetShaderiv (eglglessink->eglglesctx.vertshader[1], GL_INFO_LOG_LENGTH,
          &test);
      info_log = g_new0 (GLchar, test);
      glGetShaderInfoLog (eglglessink->eglglesctx.vertshader[1], test, NULL,
          info_log);
      GST_INFO_OBJECT (eglglessink, "Compilation info log:\n%s", info_log);
      g_free (info_log);
      goto HANDLE_ERROR;
    }

    eglglessink->eglglesctx.fragshader[1] = glCreateShader (GL_FRAGMENT_SHADER);
    glShaderSource (eglglessink->eglglesctx.fragshader[1], 1, &frag_BLACK_prog,
        NULL);

    if (got_gl_error ("glShaderSource fragment"))
      goto HANDLE_ERROR;

    glCompileShader (eglglessink->eglglesctx.fragshader[1]);
    if (got_gl_error ("glCompileShader fragment"))
      goto HANDLE_ERROR;

    glGetShaderiv (eglglessink->eglglesctx.fragshader[1], GL_COMPILE_STATUS,
        &test);
    if (test != GL_FALSE)
      GST_DEBUG_OBJECT (eglglessink, "Successfully compiled fragment shader");
    else {
      GST_ERROR_OBJECT (eglglessink, "Couldn't compile fragment shader");
      glGetShaderiv (eglglessink->eglglesctx.fragshader[1], GL_INFO_LOG_LENGTH,
          &test);
      info_log = g_new0 (GLchar, test);
      glGetShaderInfoLog (eglglessink->eglglesctx.fragshader[1], test, NULL,
          info_log);
      GST_INFO_OBJECT (eglglessink, "Compilation info log:\n%s", info_log);
      g_free (info_log);
      goto HANDLE_ERROR;
    }

    eglglessink->eglglesctx.glslprogram[1] = glCreateProgram ();
    if (got_gl_error ("glCreateProgram"))
      goto HANDLE_ERROR;
    glAttachShader (eglglessink->eglglesctx.glslprogram[1],
        eglglessink->eglglesctx.vertshader[1]);
    if (got_gl_error ("glAttachShader vertices"))
      goto HANDLE_ERROR;
    glAttachShader (eglglessink->eglglesctx.glslprogram[1],
        eglglessink->eglglesctx.fragshader[1]);
    if (got_gl_error ("glAttachShader fragments"))
      goto HANDLE_ERROR;
    glLinkProgram (eglglessink->eglglesctx.glslprogram[1]);
    glGetProgramiv (eglglessink->eglglesctx.glslprogram[1], GL_LINK_STATUS,
        &test);
    if (test != GL_FALSE)
      GST_DEBUG_OBJECT (eglglessink, "GLES: Successfully linked program");
    else {
      GST_ERROR_OBJECT (eglglessink, "Couldn't link program");
      goto HANDLE_ERROR;
    }

    eglglessink->eglglesctx.position_loc[1] =
        glGetAttribLocation (eglglessink->eglglesctx.glslprogram[1],
        "position");

    glEnableVertexAttribArray (eglglessink->eglglesctx.position_loc[1]);
    if (got_gl_error ("glEnableVertexAttribArray"))
      goto HANDLE_ERROR;
  }

  glUseProgram (eglglessink->eglglesctx.glslprogram[0]);

  /* Generate and bind texture */
  if (!eglglessink->have_texture) {
    gint i;

    GST_INFO_OBJECT (eglglessink, "Performing initial texture setup");

    for (i = 0; i < eglglessink->eglglesctx.n_textures; i++) {
      if (i == 0)
        glActiveTexture (GL_TEXTURE0);
      else if (i == 1)
        glActiveTexture (GL_TEXTURE1);
      else if (i == 2)
        glActiveTexture (GL_TEXTURE2);

      glGenTextures (1, &eglglessink->eglglesctx.texture[i]);
      if (got_gl_error ("glGenTextures"))
        goto HANDLE_ERROR_LOCKED;

      glBindTexture (GL_TEXTURE_2D, eglglessink->eglglesctx.texture[i]);
      if (got_gl_error ("glBindTexture"))
        goto HANDLE_ERROR_LOCKED;

      eglglessink->eglglesctx.tex_loc[i] =
          glGetUniformLocation (eglglessink->eglglesctx.glslprogram[0],
          texnames[i]);
      glUniform1i (eglglessink->eglglesctx.tex_loc[i], i);

      /* Set 2D resizing params */
      glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
      glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
      /* If these are not set the texture image unit will return
       * (R, G, B, A) = black on glTexImage2D for non-POT width/height
       * frames. For a deeper explanation take a look at the OpenGL ES
       * documentation for glTexParameter */
      glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
      glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
      if (got_gl_error ("glTexParameteri"))
        goto HANDLE_ERROR_LOCKED;
    }

    eglglessink->have_texture = TRUE;
  }
  glUseProgram (0);

  g_free (tmp_prog);

  return TRUE;

  /* Errors */
HANDLE_EGL_ERROR_LOCKED:
  GST_ERROR_OBJECT (eglglessink, "EGL call returned error %x", eglGetError ());
HANDLE_ERROR_LOCKED:
HANDLE_ERROR:
  GST_ERROR_OBJECT (eglglessink, "Couldn't setup EGL surface");
  g_free (tmp_prog);
  return FALSE;
}

static gboolean
gst_eglglessink_init_egl_display (GstEglGlesSink * eglglessink)
{
  GST_DEBUG_OBJECT (eglglessink, "Enter EGL initial configuration");

  eglglessink->eglglesctx.display = eglGetDisplay (EGL_DEFAULT_DISPLAY);
  if (eglglessink->eglglesctx.display == EGL_NO_DISPLAY) {
    GST_ERROR_OBJECT (eglglessink, "Could not get EGL display connection");
    goto HANDLE_ERROR;          /* No EGL error is set by eglGetDisplay() */
  }

  if (!eglInitialize (eglglessink->eglglesctx.display,
          &eglglessink->eglglesctx.egl_major,
          &eglglessink->eglglesctx.egl_minor)) {
    show_egl_error ("eglInitialize");
    GST_ERROR_OBJECT (eglglessink, "Could not init EGL display connection");
    goto HANDLE_EGL_ERROR;
  }

  /* Check against required EGL version
   * XXX: Need to review the version requirement in terms of the needed API
   */
  if (eglglessink->eglglesctx.egl_major < GST_EGLGLESSINK_EGL_MIN_VERSION) {
    GST_ERROR_OBJECT (eglglessink, "EGL v%d needed, but you only have v%d.%d",
        GST_EGLGLESSINK_EGL_MIN_VERSION, eglglessink->eglglesctx.egl_major,
        eglglessink->eglglesctx.egl_minor);
    goto HANDLE_ERROR;
  }

  GST_INFO_OBJECT (eglglessink, "System reports supported EGL version v%d.%d",
      eglglessink->eglglesctx.egl_major, eglglessink->eglglesctx.egl_minor);

  eglBindAPI (EGL_OPENGL_ES_API);

  return TRUE;

  /* Errors */
HANDLE_EGL_ERROR:
  GST_ERROR_OBJECT (eglglessink, "EGL call returned error %x", eglGetError ());
HANDLE_ERROR:
  GST_ERROR_OBJECT (eglglessink, "Couldn't setup window/surface from handle");
  return FALSE;
}

static gboolean
gst_eglglessink_choose_config (GstEglGlesSink * eglglessink)
{
  EGLint con_attribs[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
  GLint egl_configs;

  if ((eglChooseConfig (eglglessink->eglglesctx.display,
              eglglessink->selected_fmt->attribs,
              &eglglessink->eglglesctx.config, 1, &egl_configs)) == EGL_FALSE) {
    show_egl_error ("eglChooseConfig");
    GST_ERROR_OBJECT (eglglessink, "eglChooseConfig failed");
    goto HANDLE_EGL_ERROR;
  }

  if (egl_configs < 1) {
    GST_ERROR_OBJECT (eglglessink,
        "Could not find matching framebuffer config");
    goto HANDLE_ERROR;
  }

  eglglessink->eglglesctx.eglcontext =
      eglCreateContext (eglglessink->eglglesctx.display,
      eglglessink->eglglesctx.config, EGL_NO_CONTEXT, con_attribs);

  if (eglglessink->eglglesctx.eglcontext == EGL_NO_CONTEXT) {
    GST_ERROR_OBJECT (eglglessink, "Error getting context, eglCreateContext");
    goto HANDLE_EGL_ERROR;
  }

  GST_DEBUG_OBJECT (eglglessink, "EGL Context: %p",
      eglglessink->eglglesctx.eglcontext);

  return TRUE;

  /* Errors */
HANDLE_EGL_ERROR:
  GST_ERROR_OBJECT (eglglessink, "EGL call returned error %x", eglGetError ());
HANDLE_ERROR:
  GST_ERROR_OBJECT (eglglessink, "Couldn't choose an usable config");
  return FALSE;
}

static void
gst_eglglessink_set_window_handle (GstXOverlay * overlay, guintptr id)
{
  GstEglGlesSink *eglglessink = GST_EGLGLESSINK (overlay);

  g_return_if_fail (GST_IS_EGLGLESSINK (eglglessink));
  GST_DEBUG_OBJECT (eglglessink, "We got a window handle: %p", (gpointer) id);

  /* OK, we have a new window */
  GST_OBJECT_LOCK (eglglessink);
  eglglessink->eglglesctx.window = (EGLNativeWindowType) id;
  eglglessink->have_window = ((gpointer) id != NULL);
  GST_OBJECT_UNLOCK (eglglessink);

  return;
}

static void
gst_eglglessink_set_render_rectangle (GstXOverlay * overlay, gint x, gint y,
    gint width, gint height)
{
  GstEglGlesSink *eglglessink = GST_EGLGLESSINK (overlay);

  g_return_if_fail (GST_IS_EGLGLESSINK (eglglessink));

  GST_OBJECT_LOCK (eglglessink);
  if (width == -1 && height == -1) {
    /* This is the set-defaults condition according to
     * the xOverlay interface docs
     */
    gst_eglglessink_reset_display_region (eglglessink);
  } else {
    GST_OBJECT_LOCK (eglglessink);
    eglglessink->display_region.x = x;
    eglglessink->display_region.y = y;
    eglglessink->display_region.w = width;
    eglglessink->display_region.h = height;
    GST_OBJECT_UNLOCK (eglglessink);
  }

  return;
}

static void
queue_item_destroy (GstDataQueueItem * item)
{
  gst_mini_object_replace (&item->object, NULL);
  g_slice_free (GstDataQueueItem, item);
}

static GstFlowReturn
gst_eglglessink_queue_buffer (GstEglGlesSink * eglglessink, GstBuffer * buf)
{
  GstDataQueueItem *item = g_slice_new0 (GstDataQueueItem);

  item->object = GST_MINI_OBJECT_CAST (buf);
  item->size = (buf ? GST_BUFFER_SIZE (buf) : 0);
  item->duration = (buf ? GST_BUFFER_DURATION (buf) : GST_CLOCK_TIME_NONE);
  item->visible = (buf ? TRUE : FALSE);
  item->destroy = (GDestroyNotify) queue_item_destroy;

  GST_DEBUG_OBJECT (eglglessink, "Queueing buffer %" GST_PTR_FORMAT, buf);

  if (buf)
    g_mutex_lock (eglglessink->render_lock);
  if (!gst_data_queue_push (eglglessink->queue, item)) {
    g_mutex_unlock (eglglessink->render_lock);
    GST_DEBUG_OBJECT (eglglessink, "Flushing");
    return GST_FLOW_WRONG_STATE;
  }

  if (buf) {
    GST_DEBUG_OBJECT (eglglessink, "Waiting for buffer to be rendered");
    g_cond_wait (eglglessink->render_cond, eglglessink->render_lock);
    GST_DEBUG_OBJECT (eglglessink, "Buffer rendered: %s",
        gst_flow_get_name (eglglessink->last_flow));
    g_mutex_unlock (eglglessink->render_lock);
  }

  return (buf ? eglglessink->last_flow : GST_FLOW_OK);
}

/* Rendering and display */
static GstFlowReturn
gst_eglglessink_render_and_display (GstEglGlesSink * eglglessink,
    GstBuffer * buf)
{
  GstVideoRectangle frame, surface;
  gint w, h;
  guint dar_n, dar_d;

  w = GST_VIDEO_SINK_WIDTH (eglglessink);
  h = GST_VIDEO_SINK_HEIGHT (eglglessink);

  GST_DEBUG_OBJECT (eglglessink,
      "Got good buffer %p. Sink geometry is %dx%d size %d", buf, w, h,
      buf ? GST_BUFFER_SIZE (buf) : -1);

  if (buf) {
    switch (eglglessink->selected_fmt->fmt) {
      case GST_EGLGLESSINK_IMAGE_RGB888:
        glActiveTexture (GL_TEXTURE0);
        glBindTexture (GL_TEXTURE_2D, eglglessink->eglglesctx.texture[0]);
        glTexImage2D (GL_TEXTURE_2D, 0, GL_RGB, w, h, 0, GL_RGB,
            GL_UNSIGNED_BYTE, GST_BUFFER_DATA (buf));
        break;
      case GST_EGLGLESSINK_IMAGE_RGB565:
        glActiveTexture (GL_TEXTURE0);
        glBindTexture (GL_TEXTURE_2D, eglglessink->eglglesctx.texture[0]);
        glTexImage2D (GL_TEXTURE_2D, 0, GL_RGB, w, h, 0, GL_RGB,
            GL_UNSIGNED_SHORT_5_6_5, GST_BUFFER_DATA (buf));
        break;
      case GST_EGLGLESSINK_IMAGE_RGBA8888:

        switch (eglglessink->format) {
          case GST_VIDEO_FORMAT_RGBA:
          case GST_VIDEO_FORMAT_BGRA:
          case GST_VIDEO_FORMAT_ARGB:
          case GST_VIDEO_FORMAT_ABGR:
          case GST_VIDEO_FORMAT_RGBx:
          case GST_VIDEO_FORMAT_BGRx:
          case GST_VIDEO_FORMAT_xRGB:
          case GST_VIDEO_FORMAT_xBGR:
            glActiveTexture (GL_TEXTURE0);
            glBindTexture (GL_TEXTURE_2D, eglglessink->eglglesctx.texture[0]);
            glTexImage2D (GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA,
                GL_UNSIGNED_BYTE, GST_BUFFER_DATA (buf));
            break;
          case GST_VIDEO_FORMAT_AYUV:
            glActiveTexture (GL_TEXTURE0);
            glBindTexture (GL_TEXTURE_2D, eglglessink->eglglesctx.texture[0]);
            glTexImage2D (GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA,
                GL_UNSIGNED_BYTE, GST_BUFFER_DATA (buf));
            break;
          case GST_VIDEO_FORMAT_Y444:
          case GST_VIDEO_FORMAT_I420:
          case GST_VIDEO_FORMAT_YV12:
          case GST_VIDEO_FORMAT_Y42B:
          case GST_VIDEO_FORMAT_Y41B:{
            gint coffset, cw, ch;

            coffset =
                gst_video_format_get_component_offset (eglglessink->format,
                0, w, h);
            cw = gst_video_format_get_component_width (eglglessink->format,
                0, w);
            ch = gst_video_format_get_component_height (eglglessink->format,
                0, h);
            glActiveTexture (GL_TEXTURE0);
            glBindTexture (GL_TEXTURE_2D, eglglessink->eglglesctx.texture[0]);
            glTexImage2D (GL_TEXTURE_2D, 0, GL_LUMINANCE, cw, ch, 0,
                GL_LUMINANCE, GL_UNSIGNED_BYTE,
                GST_BUFFER_DATA (buf) + coffset);
            coffset =
                gst_video_format_get_component_offset (eglglessink->format,
                1, w, h);
            cw = gst_video_format_get_component_width (eglglessink->format,
                1, w);
            ch = gst_video_format_get_component_height (eglglessink->format,
                1, h);
            glActiveTexture (GL_TEXTURE1);
            glBindTexture (GL_TEXTURE_2D, eglglessink->eglglesctx.texture[1]);
            glTexImage2D (GL_TEXTURE_2D, 0, GL_LUMINANCE, cw, ch, 0,
                GL_LUMINANCE, GL_UNSIGNED_BYTE,
                GST_BUFFER_DATA (buf) + coffset);
            coffset =
                gst_video_format_get_component_offset (eglglessink->format,
                2, w, h);
            cw = gst_video_format_get_component_width (eglglessink->format,
                2, w);
            ch = gst_video_format_get_component_height (eglglessink->format,
                2, h);
            glActiveTexture (GL_TEXTURE2);
            glBindTexture (GL_TEXTURE_2D, eglglessink->eglglesctx.texture[2]);
            glTexImage2D (GL_TEXTURE_2D, 0, GL_LUMINANCE, cw, ch, 0,
                GL_LUMINANCE, GL_UNSIGNED_BYTE,
                GST_BUFFER_DATA (buf) + coffset);
            break;
          }
          case GST_VIDEO_FORMAT_YUY2:
          case GST_VIDEO_FORMAT_YVYU:
          case GST_VIDEO_FORMAT_UYVY:
            glActiveTexture (GL_TEXTURE0);
            glBindTexture (GL_TEXTURE_2D, eglglessink->eglglesctx.texture[0]);
            glTexImage2D (GL_TEXTURE_2D, 0, GL_LUMINANCE_ALPHA, w, h, 0,
                GL_LUMINANCE_ALPHA, GL_UNSIGNED_BYTE, GST_BUFFER_DATA (buf));
            glActiveTexture (GL_TEXTURE1);
            glBindTexture (GL_TEXTURE_2D, eglglessink->eglglesctx.texture[1]);
            glTexImage2D (GL_TEXTURE_2D, 0, GL_RGBA, GST_ROUND_UP_2 (w) / 2,
                h, 0, GL_RGBA, GL_UNSIGNED_BYTE, GST_BUFFER_DATA (buf));
            break;
          case GST_VIDEO_FORMAT_NV12:
          case GST_VIDEO_FORMAT_NV21:{
            gint coffset, cw, ch;

            coffset =
                gst_video_format_get_component_offset (eglglessink->format,
                0, w, h);
            cw = gst_video_format_get_component_width (eglglessink->format,
                0, w);
            ch = gst_video_format_get_component_height (eglglessink->format,
                0, h);
            glActiveTexture (GL_TEXTURE0);
            glBindTexture (GL_TEXTURE_2D, eglglessink->eglglesctx.texture[0]);
            glTexImage2D (GL_TEXTURE_2D, 0, GL_LUMINANCE, cw, ch, 0,
                GL_LUMINANCE, GL_UNSIGNED_BYTE,
                GST_BUFFER_DATA (buf) + coffset);

            coffset =
                gst_video_format_get_component_offset (eglglessink->format,
                (eglglessink->format == GST_VIDEO_FORMAT_NV12 ? 1 : 2), w, h);
            cw = gst_video_format_get_component_width (eglglessink->format, 1,
                w);
            ch = gst_video_format_get_component_height (eglglessink->format, 1,
                h);
            glActiveTexture (GL_TEXTURE1);
            glBindTexture (GL_TEXTURE_2D, eglglessink->eglglesctx.texture[1]);
            glTexImage2D (GL_TEXTURE_2D, 0, GL_LUMINANCE_ALPHA, cw, ch, 0,
                GL_LUMINANCE_ALPHA, GL_UNSIGNED_BYTE,
                GST_BUFFER_DATA (buf) + coffset);
            break;
          }
          default:
            g_assert_not_reached ();
            break;
        }
    }

    if (got_gl_error ("glTexImage2D"))
      goto HANDLE_ERROR;
  }

  /* If no one has set a display rectangle on us initialize
   * a sane default. According to the docs on the xOverlay
   * interface we are supposed to fill the overlay 100%. We
   * do this trying to take PAR/DAR into account unless the
   * calling party explicitly ask us not to by setting
   * force_aspect_ratio to FALSE.
   */
  if (gst_eglglessink_update_surface_dimensions (eglglessink) ||
      !eglglessink->display_region.w || !eglglessink->display_region.h) {
    GST_OBJECT_LOCK (eglglessink);
    if (!eglglessink->force_aspect_ratio) {
      eglglessink->display_region.x = 0;
      eglglessink->display_region.y = 0;
      eglglessink->display_region.w = eglglessink->eglglesctx.surface_width;
      eglglessink->display_region.h = eglglessink->eglglesctx.surface_height;
    } else {
      if (!gst_video_calculate_display_ratio (&dar_n, &dar_d, w, h,
              eglglessink->par_n, eglglessink->par_d,
              eglglessink->eglglesctx.pixel_aspect_ratio,
              EGL_DISPLAY_SCALING)) {
        GST_WARNING_OBJECT (eglglessink, "Could not compute resulting DAR");
        frame.w = w;
        frame.h = h;
      } else {
        /* Find suitable matching new size acording to dar & par
         * rationale for prefering leaving the height untouched
         * comes from interlacing considerations.
         * XXX: Move this to gstutils?
         */
        if (h % dar_d == 0) {
          frame.w = gst_util_uint64_scale_int (h, dar_n, dar_d);
          frame.h = h;
        } else if (w % dar_n == 0) {
          frame.h = gst_util_uint64_scale_int (w, dar_d, dar_n);
          frame.w = w;
        } else {
          /* Neither width nor height can be precisely scaled.
           * Prefer to leave height untouched. See comment above.
           */
          frame.w = gst_util_uint64_scale_int (h, dar_n, dar_d);
          frame.h = h;
        }
      }

      surface.w = eglglessink->eglglesctx.surface_width;
      surface.h = eglglessink->eglglesctx.surface_height;
      gst_video_sink_center_rect (frame, surface,
          &eglglessink->display_region, TRUE);
    }
    GST_OBJECT_UNLOCK (eglglessink);

    glViewport (0, 0,
        eglglessink->eglglesctx.surface_width,
        eglglessink->eglglesctx.surface_height);

    /* Clear the surface once if its content is preserved */
    if (eglglessink->eglglesctx.buffer_preserved) {
      glClearColor (0.0, 0.0, 0.0, 1.0);
      glClear (GL_COLOR_BUFFER_BIT);
    }

    if (!gst_eglglessink_setup_vbo (eglglessink, FALSE)) {
      GST_ERROR_OBJECT (eglglessink, "VBO setup failed");
      goto HANDLE_ERROR;
    }
  }

  if (!eglglessink->eglglesctx.buffer_preserved) {
    /* Draw black borders */
    GST_DEBUG_OBJECT (eglglessink, "Drawing black border 1");
    glUseProgram (eglglessink->eglglesctx.glslprogram[1]);

    glVertexAttribPointer (eglglessink->eglglesctx.position_loc[1], 3,
        GL_FLOAT, GL_FALSE, sizeof (coord5), (gpointer) (4 * sizeof (coord5)));
    if (got_gl_error ("glVertexAttribPointer"))
      goto HANDLE_ERROR;

    glDrawElements (GL_TRIANGLE_STRIP, 4, GL_UNSIGNED_SHORT, 0);
    if (got_gl_error ("glDrawElements"))
      goto HANDLE_ERROR;

    GST_DEBUG_OBJECT (eglglessink, "Drawing black border 2");

    glVertexAttribPointer (eglglessink->eglglesctx.position_loc[1], 3,
        GL_FLOAT, GL_FALSE, sizeof (coord5), (gpointer) (8 * sizeof (coord5)));
    if (got_gl_error ("glVertexAttribPointer"))
      goto HANDLE_ERROR;

    glDrawElements (GL_TRIANGLE_STRIP, 4, GL_UNSIGNED_SHORT, 0);
    if (got_gl_error ("glDrawElements"))
      goto HANDLE_ERROR;
  }

  /* Draw video frame */
  GST_DEBUG_OBJECT (eglglessink, "Drawing video frame");
  glUseProgram (eglglessink->eglglesctx.glslprogram[0]);

  glVertexAttribPointer (eglglessink->eglglesctx.position_loc[0], 3,
      GL_FLOAT, GL_FALSE, sizeof (coord5), (gpointer) (0 * sizeof (coord5)));
  if (got_gl_error ("glVertexAttribPointer"))
    goto HANDLE_ERROR;

  glVertexAttribPointer (eglglessink->eglglesctx.texpos_loc, 2, GL_FLOAT,
      GL_FALSE, sizeof (coord5), (gpointer) (3 * sizeof (gfloat)));
  if (got_gl_error ("glVertexAttribPointer"))
    goto HANDLE_ERROR;

  glDrawElements (GL_TRIANGLE_STRIP, 4, GL_UNSIGNED_SHORT, 0);
  if (got_gl_error ("glDrawElements"))
    goto HANDLE_ERROR;

  if ((eglSwapBuffers (eglglessink->eglglesctx.display,
              eglglessink->eglglesctx.surface))
      == EGL_FALSE) {
    show_egl_error ("eglSwapBuffers");
    goto HANDLE_ERROR;
  }

  GST_DEBUG_OBJECT (eglglessink, "Succesfully rendered 1 frame");
  return GST_FLOW_OK;

HANDLE_ERROR:
  GST_ERROR_OBJECT (eglglessink, "Rendering disabled for this frame");

  return GST_FLOW_ERROR;
}

static GstFlowReturn
gst_eglglessink_show_frame (GstVideoSink * vsink, GstBuffer * buf)
{
  GstEglGlesSink *eglglessink;

  g_return_val_if_fail (buf != NULL, GST_FLOW_ERROR);

  eglglessink = GST_EGLGLESSINK (vsink);
  GST_DEBUG_OBJECT (eglglessink, "Got buffer: %p", buf);

  buf = gst_buffer_make_metadata_writable (gst_buffer_ref (buf));
  gst_buffer_set_caps (buf, eglglessink->current_caps);
  return gst_eglglessink_queue_buffer (eglglessink, buf);
}

static GstCaps *
gst_eglglessink_getcaps (GstBaseSink * bsink)
{
  GstEglGlesSink *eglglessink;
  GstCaps *ret = NULL;

  eglglessink = GST_EGLGLESSINK (bsink);

  GST_OBJECT_LOCK (eglglessink);
  if (eglglessink->sinkcaps) {
    ret = gst_caps_ref (eglglessink->sinkcaps);
  } else {
    ret =
        gst_caps_copy (gst_pad_get_pad_template_caps (GST_BASE_SINK_PAD
            (bsink)));
  }
  GST_OBJECT_UNLOCK (eglglessink);

  return ret;
}

static gboolean
gst_eglglessink_configure_caps (GstEglGlesSink * eglglessink, GstCaps * caps)
{
  gboolean ret = TRUE;
  gint width, height;
  int par_n, par_d;
  GstEglGlesImageFmt *format;

  if (!(ret = gst_video_format_parse_caps (caps, &eglglessink->format, &width,
              &height))) {
    GST_ERROR_OBJECT (eglglessink, "Got weird and/or incomplete caps");
    goto HANDLE_ERROR;
  }

  if (!(ret = gst_video_parse_caps_pixel_aspect_ratio (caps, &par_n, &par_d))) {
    par_n = 1;
    par_d = 1;
    GST_WARNING_OBJECT (eglglessink,
        "Can't parse PAR from caps. Using default: 1");
  }

  format = gst_eglglessink_get_compat_format_from_caps (eglglessink, caps);
  if (!format) {
    GST_ERROR_OBJECT (eglglessink,
        "No supported and compatible EGL/GLES format found for given caps");
    goto HANDLE_ERROR;
  } else
    GST_INFO_OBJECT (eglglessink, "Selected compatible EGL/GLES format %d",
        format->fmt);

  eglglessink->selected_fmt = format;
  eglglessink->par_n = par_n;
  eglglessink->par_d = par_d;
  GST_VIDEO_SINK_WIDTH (eglglessink) = width;
  GST_VIDEO_SINK_HEIGHT (eglglessink) = height;

  if (eglglessink->configured_caps) {
    GST_ERROR_OBJECT (eglglessink, "Caps were already set");
    if (gst_caps_can_intersect (caps, eglglessink->configured_caps)) {
      GST_INFO_OBJECT (eglglessink, "Caps are compatible anyway");
      goto SUCCEED;
    }

    GST_DEBUG_OBJECT (eglglessink, "Caps are not compatible, reconfiguring");

    /* EGL/GLES cleanup */
    gst_eglglessink_wipe_eglglesctx (eglglessink);

    gst_caps_unref (eglglessink->configured_caps);
    eglglessink->configured_caps = NULL;
  }

  if (!gst_eglglessink_choose_config (eglglessink)) {
    GST_ERROR_OBJECT (eglglessink, "Couldn't choose EGL config");
    goto HANDLE_ERROR;
  }

  gst_caps_replace (&eglglessink->configured_caps, caps);

  /* By now the application should have set a window
   * if it meant to do so
   */
  GST_OBJECT_LOCK (eglglessink);
  if (!eglglessink->have_window) {
    EGLNativeWindowType window;

    GST_INFO_OBJECT (eglglessink,
        "No window. Will attempt internal window creation");
    if (!(window = gst_eglglessink_create_window (eglglessink, width, height))) {
      GST_ERROR_OBJECT (eglglessink, "Internal window creation failed!");
      GST_OBJECT_UNLOCK (eglglessink);
      goto HANDLE_ERROR;
    }
    eglglessink->using_own_window = TRUE;
    eglglessink->eglglesctx.window = window;
    eglglessink->have_window = TRUE;
  }
  GST_DEBUG_OBJECT (eglglessink, "Using window handle %p",
      eglglessink->eglglesctx.window);
  eglglessink->eglglesctx.used_window = eglglessink->eglglesctx.window;
  GST_OBJECT_UNLOCK (eglglessink);
  gst_x_overlay_got_window_handle (GST_X_OVERLAY (eglglessink),
      (guintptr) eglglessink->eglglesctx.used_window);

  if (!eglglessink->have_surface) {
    if (!gst_eglglessink_init_egl_surface (eglglessink)) {
      GST_ERROR_OBJECT (eglglessink, "Couldn't init EGL surface from window");
      goto HANDLE_ERROR;
    }
  }

SUCCEED:
  GST_INFO_OBJECT (eglglessink, "Configured caps successfully");
  return TRUE;

HANDLE_ERROR:
  GST_ERROR_OBJECT (eglglessink, "Configuring caps failed");
  return FALSE;
}

static gboolean
gst_eglglessink_setcaps (GstBaseSink * bsink, GstCaps * caps)
{
  GstEglGlesSink *eglglessink;

  eglglessink = GST_EGLGLESSINK (bsink);

  GST_DEBUG_OBJECT (eglglessink,
      "Current caps %" GST_PTR_FORMAT ", setting caps %"
      GST_PTR_FORMAT, eglglessink->current_caps, caps);

  gst_caps_replace (&eglglessink->current_caps, caps);

  return TRUE;
}

static void
gst_eglglessink_wipe_fmt (gpointer data)
{
  GstEglGlesImageFmt *format = data;
  gst_caps_unref (format->caps);
  g_free (format);
}

static gboolean
gst_eglglessink_open (GstEglGlesSink * eglglessink)
{
  if (!egl_init (eglglessink)) {
    return FALSE;
  }

  return TRUE;
}

static gboolean
gst_eglglessink_close (GstEglGlesSink * eglglessink)
{
  if (eglglessink->eglglesctx.display) {
    eglTerminate (eglglessink->eglglesctx.display);
    eglglessink->eglglesctx.display = NULL;
  }

  eglglessink->selected_fmt = NULL;
  g_list_free_full (eglglessink->supported_fmts, gst_eglglessink_wipe_fmt);
  eglglessink->supported_fmts = NULL;
  gst_caps_unref (eglglessink->sinkcaps);
  eglglessink->sinkcaps = NULL;
  eglglessink->egl_started = FALSE;

  return TRUE;
}

static GstStateChangeReturn
gst_eglglessink_change_state (GstElement * element, GstStateChange transition)
{
  GstEglGlesSink *eglglessink;
  GstStateChangeReturn ret = GST_STATE_CHANGE_SUCCESS;

  eglglessink = GST_EGLGLESSINK (element);

  switch (transition) {
    case GST_STATE_CHANGE_NULL_TO_READY:
      if (!gst_eglglessink_open (eglglessink)) {
        ret = GST_STATE_CHANGE_FAILURE;
        goto done;
      }
      break;
    case GST_STATE_CHANGE_READY_TO_PAUSED:
      if (!gst_eglglessink_start (eglglessink)) {
        ret = GST_STATE_CHANGE_FAILURE;
        goto done;
      }
      break;
    default:
      break;
  }

  ret = GST_ELEMENT_CLASS (parent_class)->change_state (element, transition);
  if (ret == GST_STATE_CHANGE_FAILURE)
    return ret;

  switch (transition) {
    case GST_STATE_CHANGE_READY_TO_NULL:
      if (!gst_eglglessink_close (eglglessink)) {
        ret = GST_STATE_CHANGE_FAILURE;
        goto done;
      }
      break;
    case GST_STATE_CHANGE_PAUSED_TO_READY:
      if (!gst_eglglessink_stop (eglglessink)) {
        ret = GST_STATE_CHANGE_FAILURE;
        goto done;
      }
      break;
    default:
      break;
  }

done:
  return ret;
}

static void
gst_eglglessink_finalize (GObject * object)
{
  GstEglGlesSink *eglglessink;

  g_return_if_fail (GST_IS_EGLGLESSINK (object));

  eglglessink = GST_EGLGLESSINK (object);

  if (eglglessink->queue)
    g_object_unref (eglglessink->queue);
  eglglessink->queue = NULL;

  if (eglglessink->render_cond)
    g_cond_free (eglglessink->render_cond);
  eglglessink->render_cond = NULL;
  if (eglglessink->render_lock);
  g_mutex_free (eglglessink->render_lock);
  eglglessink->render_lock = NULL;

  G_OBJECT_CLASS (parent_class)->finalize (object);
}

static void
gst_eglglessink_set_property (GObject * object, guint prop_id,
    const GValue * value, GParamSpec * pspec)
{
  GstEglGlesSink *eglglessink;

  g_return_if_fail (GST_IS_EGLGLESSINK (object));

  eglglessink = GST_EGLGLESSINK (object);

  switch (prop_id) {
    case PROP_CREATE_WINDOW:
      eglglessink->create_window = g_value_get_boolean (value);
      break;
    case PROP_FORCE_ASPECT_RATIO:
      eglglessink->force_aspect_ratio = g_value_get_boolean (value);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static void
gst_eglglessink_get_property (GObject * object, guint prop_id,
    GValue * value, GParamSpec * pspec)
{
  GstEglGlesSink *eglglessink;

  g_return_if_fail (GST_IS_EGLGLESSINK (object));

  eglglessink = GST_EGLGLESSINK (object);

  switch (prop_id) {
    case PROP_CREATE_WINDOW:
      g_value_set_boolean (value, eglglessink->create_window);
      break;
    case PROP_FORCE_ASPECT_RATIO:
      g_value_set_boolean (value, eglglessink->force_aspect_ratio);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static void
gst_eglglessink_base_init (gpointer gclass)
{
  GstElementClass *element_class = GST_ELEMENT_CLASS (gclass);

  gst_element_class_set_details_simple (element_class,
      "EGL/GLES vout Sink",
      "Sink/Video",
      "An EGL/GLES Video Output Sink Implementing the XOverlay interface",
      "Reynaldo H. Verdejo Pinochet <reynaldo@collabora.com>, "
      "Sebastian Dröge <sebastian.droege@collabora.co.uk>");

  gst_element_class_add_pad_template (element_class,
      gst_static_pad_template_get (&gst_eglglessink_sink_template_factory));
}

/* initialize the eglglessink's class */
static void
gst_eglglessink_class_init (GstEglGlesSinkClass * klass)
{
  GObjectClass *gobject_class;
  GstElementClass *gstelement_class;
  GstBaseSinkClass *gstbasesink_class;
  GstVideoSinkClass *gstvideosink_class;

  gobject_class = (GObjectClass *) klass;
  gstelement_class = (GstElementClass *) klass;
  gstbasesink_class = (GstBaseSinkClass *) klass;
  gstvideosink_class = (GstVideoSinkClass *) klass;

  gobject_class->set_property = gst_eglglessink_set_property;
  gobject_class->get_property = gst_eglglessink_get_property;
  gobject_class->finalize = gst_eglglessink_finalize;

  gstelement_class->change_state = gst_eglglessink_change_state;

  gstbasesink_class->set_caps = GST_DEBUG_FUNCPTR (gst_eglglessink_setcaps);
  gstbasesink_class->get_caps = GST_DEBUG_FUNCPTR (gst_eglglessink_getcaps);

  gstvideosink_class->show_frame =
      GST_DEBUG_FUNCPTR (gst_eglglessink_show_frame);

  g_object_class_install_property (gobject_class, PROP_CREATE_WINDOW,
      g_param_spec_boolean ("create-window", "Create Window",
          "If set to true, the sink will attempt to create it's own window to "
          "render to if none is provided. This is currently only supported "
          "when the sink is used under X11",
          TRUE, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
  g_object_class_install_property (gobject_class, PROP_FORCE_ASPECT_RATIO,
      g_param_spec_boolean ("force-aspect-ratio",
          "Respect aspect ratio when scaling",
          "If set to true, the sink will attempt to preserve the incoming "
          "frame's geometry while scaling, taking both the storage's and "
          "display's pixel aspect ratio into account",
          TRUE, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
}

static gboolean
queue_check_full_func (GstDataQueue * queue, guint visible, guint bytes,
    guint64 time, gpointer checkdata)
{
  return visible != 0;
}

static void
gst_eglglessink_init (GstEglGlesSink * eglglessink,
    GstEglGlesSinkClass * gclass)
{
  /* Init defaults */

  /** Flags */
  eglglessink->have_window = FALSE;
  eglglessink->have_surface = FALSE;
  eglglessink->have_vbo = FALSE;
  eglglessink->have_texture = FALSE;
  eglglessink->egl_started = FALSE;
  eglglessink->using_own_window = FALSE;

  /** Props */
  eglglessink->create_window = TRUE;
  eglglessink->force_aspect_ratio = TRUE;

  eglglessink->par_n = 1;
  eglglessink->par_d = 1;

  eglglessink->render_lock = g_mutex_new ();
  eglglessink->render_cond = g_cond_new ();
  eglglessink->queue = gst_data_queue_new (queue_check_full_func, NULL);
  eglglessink->last_flow = GST_FLOW_WRONG_STATE;
}

/* Interface initializations. Used here for initializing the XOverlay
 * Interface.
 */
static void
gst_eglglessink_init_interfaces (GType type)
{
  static const GInterfaceInfo implements_info = {
    (GInterfaceInitFunc) gst_eglglessink_implements_init, NULL, NULL
  };

  static const GInterfaceInfo xoverlay_info = {
    (GInterfaceInitFunc) gst_eglglessink_xoverlay_init, NULL, NULL
  };

  g_type_add_interface_static (type, GST_TYPE_IMPLEMENTS_INTERFACE,
      &implements_info);
  g_type_add_interface_static (type, GST_TYPE_X_OVERLAY, &xoverlay_info);

}

/* entry point to initialize the plug-in
 * initialize the plug-in itself
 * register the element factories and other features
 */
static gboolean
eglglessink_plugin_init (GstPlugin * plugin)
{
  /* debug category for fltering log messages */
  GST_DEBUG_CATEGORY_INIT (gst_eglglessink_debug, "eglglessink",
      0, "Simple EGL/GLES Sink");

  return gst_element_register (plugin, "eglglessink", GST_RANK_PRIMARY,
      GST_TYPE_EGLGLESSINK);
}

/* gstreamer looks for this structure to register eglglessinks */
GST_PLUGIN_DEFINE (GST_VERSION_MAJOR,
    GST_VERSION_MINOR,
    "eglglessink",
    "EGL/GLES sink",
    eglglessink_plugin_init,
    VERSION, "LGPL", "GStreamer", "http://gstreamer.net/")