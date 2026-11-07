/* WineMetalGL driver ABI: the C interface the Wine module (src/wine/unix)
 * uses to drive the WineMetalGL core (libwinemetalgl.dylib). The module loads
 * the library with dlopen and resolves these symbols with dlsym; GL entry points are obtained
 * through wmgl_get_proc_address, never as exported symbols.
 *
 * Every function is called on the Unix side of Wine. Pointers are host
 * pointers. Contexts and surfaces are opaque handles owned by the core.
 *
 * Bump WMGL_DRIVER_ABI_VERSION whenever a struct or signature changes; the
 * core refuses to initialise for a different version.
 */
#ifndef WINEMETALGL_DRIVER_H
#define WINEMETALGL_DRIVER_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define WMGL_DRIVER_ABI_VERSION 1

struct wmgl_pixel_format
{
    int draw_to_window;
    int draw_to_pbuffer;
    int double_buffer;
    int stereo;
    int color_bits;        /* cColorBits, WGL_COLOR_BITS_ARB: red + green + blue, and alpha where the format
                              has it, as Windows drivers count them (32 for RGBA8) */
    int red_bits, green_bits, blue_bits, alpha_bits;
    int red_shift, green_shift, blue_shift, alpha_shift;
    int depth_bits;
    int stencil_bits;
    int accum_bits;        /* total accumulation bits (compatibility profile) */
    int aux_buffers;
    int sample_buffers;
    int samples;
    int srgb_capable;
    int float_components;
    int max_pbuffer_width, max_pbuffer_height, max_pbuffer_pixels;
};

/* Context creation errors reported through *error. */
#define WMGL_ERROR_NONE             0
#define WMGL_ERROR_INVALID_VERSION  1  /* ERROR_INVALID_VERSION_ARB */
#define WMGL_ERROR_INVALID_PROFILE  2  /* ERROR_INVALID_PROFILE_ARB */
#define WMGL_ERROR_INVALID_ATTRIB   3
#define WMGL_ERROR_BAD_SHARE        4

/* Flags for wmgl_surface_flush (mirror win32u's GL_FLUSH_* bits). */
#define WMGL_FLUSH_FINISHED  0x01
#define WMGL_FLUSH_INTERVAL  0x02
#define WMGL_FLUSH_UPDATED   0x04
#define WMGL_FLUSH_PRESENT   0x08

typedef struct wmgl_context wmgl_context;
typedef struct wmgl_surface wmgl_surface;

/* Returns 0 on success. */
int wmgl_initialize(unsigned int abi_version);

/* Fills up to max formats; returns the total count. *onscreen receives the
 * number of formats (at the start of the list) usable for windows. Format
 * numbers passed back to the core are 1-based indices into this list. */
unsigned int wmgl_get_pixel_formats(struct wmgl_pixel_format *formats, unsigned int max, unsigned int *onscreen);

/* layer is a CAMetalLayer*; width/height are in device pixels. */
wmgl_surface *wmgl_surface_create_window(void *layer, int format, int width, int height);
wmgl_surface *wmgl_surface_create_pbuffer(int format, int width, int height);
void wmgl_surface_destroy(wmgl_surface *surface);
void wmgl_surface_flush(wmgl_surface *surface, unsigned int flags, int width, int height, int interval);
int wmgl_surface_swap(wmgl_surface *surface);
/* The size a window surface is shown at, in device pixels (0, 0: its own):
 * the back buffer is scaled up to it (MetalFX spatial) when it is larger. */
void wmgl_surface_set_output(wmgl_surface *surface, int width, int height);

/* attribs: WGL_CONTEXT_* name/value pairs terminated by 0, may be NULL. */
wmgl_context *wmgl_context_create(int format, wmgl_context *share, const int *attribs, int *error);
void wmgl_context_destroy(wmgl_context *context);
/* Makes the context current on the calling thread (NULL releases). */
int wmgl_make_current(wmgl_context *context, wmgl_surface *draw, wmgl_surface *read);

void *wmgl_get_proc_address(const char *name);

/* Renderer information for WGL_WINE_query_renderer. */
const char *wmgl_renderer_string(void);
unsigned int wmgl_video_memory_mb(void);
void wmgl_max_versions(int *core_major, int *core_minor, int *compat_major, int *compat_minor);
/* The highest OpenGL ES version of contexts created with
 * WGL_CONTEXT_ES_PROFILE_BIT_EXT. Optional: cores before OpenGL ES do not
 * export it. */
void wmgl_max_es_version(int *major, int *minor);
/* WineMetalGL's own version (WGL_RENDERER_VERSION_WINE). Optional: cores
 * before 2.0.0 do not export it. */
void wmgl_version(int *major, int *minor, int *patch);
/* Memory for buffer objects' storage, for hosts whose clients see part of
 * the address space only (32-bit Windows applications under Wine's wow64,
 * which must be able to address mapped buffers): `allocate` returns memory
 * of at least `size` bytes aligned to 64 KiB (NULL on failure), `release`
 * frees it. Call before creating contexts. Optional: cores before 3.0.0 do
 * not export it. */
void wmgl_set_buffer_memory(void *(*allocate)(size_t size), void (*release)(void *memory, size_t size));
/* Diagnostics, for tests: the WINEMETALGL_STATS counters of the calling
 * thread (src/core/stats.h). wmgl_stats_enable(1) counts without logging
 * (0 stops counting unless WINEMETALGL_STATS is set), wmgl_stats_reset zeroes
 * the counters, and wmgl_stats_counter reads one by name ("draws", "waits",
 * "commits", "indirectGpu", "tessMetal", "passEnds.commit", ...), returning
 * 0 for an unknown name. Optional: cores before 3.2.0 do not export them. */
/* One autorelease pool around a batch of GL calls instead of one per call
 * (the Wine module's batches, src/wine/unix/unix_main.c). */
void wmgl_batch_begin(void);
void wmgl_batch_end(void);

void wmgl_stats_enable(int enable);
void wmgl_stats_reset(void);
int wmgl_stats_counter(const char *name, unsigned long long *value);
/* Logging (src/core/util/log.h), so that the Wine module's messages share
 * the core's levels, log file and crash history. wmgl_log_set_host names the
 * program (the Windows executable's path) and the host (Wine's version) for
 * the banner and the log file's name; call it before wmgl_initialize.
 * wmgl_log_level returns the highest level (0 error .. 3 trace, -1 none) that
 * a category ("wine", "pe", ...; unknown names are "wine") keeps, so callers
 * can skip building messages; wmgl_log_write logs one message (a trailing
 * newline is dropped). Optional: cores before 3.3.0 do not export them. */
void wmgl_log_set_host(const char *program, const char *host_version);
int wmgl_log_level(const char *category);
void wmgl_log_write(int level, const char *category, const char *text);

#ifdef __cplusplus
}
#endif

#endif /* WINEMETALGL_DRIVER_H */
