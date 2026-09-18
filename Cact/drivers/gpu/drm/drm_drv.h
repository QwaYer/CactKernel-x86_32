#ifndef DRM_DRV_H
#define DRM_DRV_H

/*
 * drm_drv.h — the kernel-side DRM/KMS core: the API a hardware driver (built-in
 * or an out-of-tree .cctk module) implements, plus the object model it fills in.
 *
 * Layering, and why it is split this way:
 *
 *   rust_drm/        the core itself (Rust crate `cact_drm`): device and object
 *                    model, GEM, KMS, ioctl decoding.  This is the dominant
 *                    part of the directory — the C files below shrink to the
 *                    ABI and VFS glue as slices move into it.
 *   uapi/            the userspace ABI: ioctl numbers and struct layouts, plus
 *                    the drm_types.h / drm_ioctl.h shims they need.  Keeping
 *                    the numbers and layouts fixed is what lets libdrm and
 *                    Mesa talk to this kernel.
 *   drm_drv.h        this file: what a driver sees.  Exported to modules through
 *                    the ksym table, and kept C-parseable because the driver is
 *                    a separate C `.cctk`.
 *   drm_internal.h   the C view of the shared state; drivers must not touch it.
 *   core/            C that remains: /dev/dri registration (devfs) and the VFS
 *                    node/file operations that hand file_t to the Rust core.
 *   gem/ kms/        C that remains: the ioctl handlers not yet moved to Rust.
 *
 * A driver never talks to userspace: it registers a drm_driver_ops_t, creates
 * KMS objects, and implements the handful of operations the core calls (mode
 * set, page flip, dirty, GEM create/free, optional private ioctls).  Everything
 * else — ioctl decoding, handle tables, mmap offsets, mode/EDID marshalling —
 * lives in the core.
 *
 * Object IDs are small per-type integers starting at 1, exactly as userspace
 * treats them (crtc_id, connector_id, … are independent namespaces).  The
 * DRM_MODE_OBJECT_* constants below identify which namespace a property ioctl
 * addresses.
 */

#include <stdint.h>
#include "drm.h"
#include "drm_mode.h"
#include "drm_fourcc.h"

/* ------------------------------------------------------------------ limits */

/*
 * There are no capacity limits any more.  Devices, CRTCs, connectors, planes,
 * framebuffers, GEM handles, mmap offsets, properties and queued events are all
 * held in allocator-backed core structures, so a device may register as many as
 * it likes and a client may hold as many as it likes.  The EDID a connector
 * carries is a blob of whatever length the driver hands over, too.
 *
 * What remains here is one fixed *size*, not a pool: the length of the mode
 * name a uapi `struct drm_mode_modeinfo` carries.
 */
#define DRM_MODE_NAME_LEN    32

/* Connector status.  The vendored uapi drm_mode.h only mentions
 * enum drm_connector_status in a doc comment; these values live in libdrm's
 * xf86drmMode.h, which is what userspace compares against. */
#define DRM_MODE_CONNECTED         1
#define DRM_MODE_DISCONNECTED      2
#define DRM_MODE_UNKNOWNCONNECTION 3

/* Plane types (uapi enum drm_plane_type; the vendored drm_mode.h does not carry
 * the enum, and both the core and a driver need the values). */
#define DRM_PLANE_TYPE_OVERLAY     0
#define DRM_PLANE_TYPE_PRIMARY     1
#define DRM_PLANE_TYPE_CURSOR      2

/* DRM_MODE_PAGE_FLIP_* come from the uapi header. */

struct drm_device;
struct drm_file;
struct drm_gem_object;

/* ------------------------------------------------------------------- modes */

struct drm_display_mode {
    uint32_t clock;            /* kHz */
    uint16_t hdisplay;
    uint16_t hsync_start;
    uint16_t hsync_end;
    uint16_t htotal;
    uint16_t hskew;
    uint16_t vdisplay;
    uint16_t vsync_start;
    uint16_t vsync_end;
    uint16_t vtotal;
    uint16_t vscan;            /* vrefresh for the userspace ioctl */
    uint32_t flags;            /* DRM_MODE_FLAG_* */
    uint32_t type;             /* DRM_MODE_TYPE_* */
    char     name[DRM_MODE_NAME_LEN];
};

/* ------------------------------------------------------------ KMS objects */

struct drm_crtc {
    struct drm_device     *dev;
    uint32_t               id;
    int                    index;
    char                   name[DRM_MODE_NAME_LEN];

    struct drm_framebuffer *fb;      /* current scanout, NULL when disabled  */
    struct drm_display_mode mode;
    int                    enabled;
    int                    x, y;     /* source position in fb                */

    uint32_t               vblank_count;
    int                    vblank_enabled;
    struct drm_connector  *connector; /* connected connector, for drivers    */
};

struct drm_encoder {
    struct drm_device *dev;
    uint32_t           id;
    int                index;
    uint32_t           encoder_type;   /* DRM_MODE_ENCODER_*                  */
    uint32_t           possible_crtcs; /* bitmask, bit i = crtcs[i]           */
    uint32_t           possible_clones;
    struct drm_crtc   *crtc;           /* attached CRTC, NULL when detached   */
};

struct drm_connector {
    struct drm_device     *dev;
    uint32_t               id;
    int                    index;
    uint32_t               connector_type;    /* DRM_MODE_CONNECTOR_*         */
    uint32_t               connector_type_id; /* e.g. VIRTUAL-1               */
    uint32_t               status;            /* DRM_MODE_CONNECTED / …       */
    uint32_t               mm_width, mm_height;

    /*
     * The mode list and the EDID block are core-owned and variable length:
     * `drm_connector_add_mode()` appends to the first and
     * `drm_connector_set_edid()` replaces the second, and neither is capped.
     * These two counts are how a driver learns what the core holds.
     */
    int                    count_modes;
    uint32_t               edid_len;

    struct drm_encoder    *encoder;     /* attached encoder, for drivers      */
    uint32_t               dpms;        /* connector DPMS property            */
};

struct drm_plane {
    struct drm_device *dev;
    uint32_t           id;
    int                index;
    uint32_t           plane_type;      /* DRM_PLANE_TYPE_*                   */
    uint32_t           possible_crtcs;
    /* The fourcc list is core-owned and variable length; not capped. */
    int                format_count;
    uint32_t           format_type;     /* DRM_FORMAT_TYPE_* mask            */
};

struct drm_framebuffer {
    struct drm_device     *dev;
    uint32_t               id;
    uint32_t               width, height, pitch;
    uint32_t               format;      /* fourcc                              */
    uint32_t               flags;
    uint32_t               modifier;    /* DRM_FORMAT_MOD_* (0 = linear)       */
    struct drm_gem_object *obj;         /* backing object                      */
    uint32_t               offset;      /* byte offset into obj                */
};

/* -------------------------------------------------------------------- GEM */

struct drm_gem_object {
    struct drm_device *dev;
    uint32_t           size;
    int                refcount;

    /* Storage is a memfd-backed shared object (see fs/memfd).  Keeping GEM on
     * top of the existing memfd machinery is what makes mmap(), munmap(),
     * fork(), and PRIME all work without a second frame-sharing mechanism:
     * the object already has an owner, a reference count, a page array and a
     * MAP_SHARED accounting hook. */
    int                memfd;

    /* Dumb-buffer description (drm_mode_create_dumb), 0 for non-dumb objects */
    uint32_t           width, height, bpp, pitch;
    int                is_dumb;

    uint32_t           map_offset;   /* faked offset for DRM_IOCTL_MODE_MAP_DUMB */
};

/*
 * The non-atomic modeset request: one CRTC, one framebuffer, one mode and the
 * connector set that scans it out.  This is what DRM_IOCTL_MODE_SETCRTC
 * decodes into and what a driver's set_config() receives.
 */
struct drm_mode_set {
    struct drm_framebuffer *fb;
    struct drm_crtc        *crtc;
    struct drm_display_mode mode;
    int                     x, y;
    /* Core-owned list, valid only for the duration of the call. */
    struct drm_connector  **connectors;
    int                     num_connectors;
};

/* ------------------------------------------------------------ driver ops */

/*
 * Operations the core calls into the driver.  All of them are optional: a
 * driver that only reports modes (and lets the core keep the framebuffer as
 * plain memory) leaves mode_set/page_flip NULL.
 */
typedef struct drm_driver_ops {
    const char *name;          /* driver name reported by DRM_IOCTL_VERSION */
    const char *desc;          /* free-form description                     */
    uint32_t    major, minor, patchlevel;
    uint32_t    driver_date;   /* compacted build date, DRM_VERSION-style   */

    /* Called after the device node exists and its KMS objects are in place.
     * The driver creates its CRTCs/connectors/encoders/planes here. */
    int  (*load)  (struct drm_device *dev);
    void (*unload)(struct drm_device *dev);

    /* KMS.  mode_valid() prunes the connector's mode list; set_config() is
     * DRM_IOCTL_MODE_SETCRTC (NULL means "the core just records the state");
     * page_flip() is DRM_IOCTL_MODE_PAGE_FLIP, called with the new fb already
     * installed in crtc->fb so a driver that cannot flip synchronously can
     * simply return 0. */
    int  (*mode_valid)(struct drm_connector *conn,
                       const struct drm_display_mode *mode);
    int  (*set_config)(struct drm_device *dev, struct drm_mode_set *set);
    int  (*page_flip)(struct drm_crtc *crtc, struct drm_framebuffer *fb,
                      uint32_t flags, void *user_data);
    int  (*dirty)     (struct drm_framebuffer *fb,
                       const struct drm_clip_rect *clips, uint32_t num_clips);

    /* Vblank accounting.  A driver with a real vblank interrupt enables one
     * here and calls drm_crtc_handle_vblank() from its handler; drivers
     * without one leave these NULL and the core synthesises vblanks from the
     * scheduler tick while a client waits. */
    int  (*enable_vblank) (struct drm_device *dev, struct drm_crtc *crtc);
    void (*disable_vblank)(struct drm_device *dev, struct drm_crtc *crtc);

    /* GEM.  gem_create() lets a driver refuse or add its own constraints
     * (e.g. a device that needs physically contiguous scanout); gem_free() is
     * the last-reference notification.  Both optional. */
    int  (*gem_create)(struct drm_device *dev, struct drm_gem_object *obj);
    void (*gem_free)  (struct drm_gem_object *obj);

    /* Last chance to release per-open state — a 3D driver's contexts, say.
     * Called while the client's GEM handles and framebuffers are still alive,
     * so the driver may still unref whatever it referenced. */
    void (*close)(struct drm_device *dev, struct drm_file *file);

    /* Driver-private ioctls, i.e. everything above DRM_COMMAND_BASE that the
     * core does not implement itself — for virtio-gpu that is the whole
     * DRM_IOCTL_VIRTGPU_* range and the 3D path (resource create, context
     * init, execbuffer).  `data` is a kernel copy of the userspace struct and
     * `size` its length (from _IOC_SIZE of the ioctl number), so the handler
     * can be told apart from a mismatched caller.  Return 0 on success. */
    int  (*ioctl)(struct drm_device *dev, struct drm_file *file,
                  uint32_t cmd, void *data, uint32_t size);

    /* Cursor.  Appended after ioctl so the offsets of everything above stay
     * put.  cursor_set() takes the cursor image (NULL hides it) and its hot
     * spot; cursor_move() only moves it.  A driver without a hardware cursor
     * leaves both NULL and the core just records the state. */
    int  (*cursor_set) (struct drm_crtc *crtc, struct drm_gem_object *obj,
                        uint32_t width, uint32_t height,
                        int32_t hot_x, int32_t hot_y);
    int  (*cursor_move)(struct drm_crtc *crtc, int32_t x, int32_t y);
} drm_driver_ops_t;

/* ------------------------------------------------------- device lifecycle */

/* Allocate a DRM device.  `priv` is the driver's own per-device state and is
 * returned by drm_dev_priv().  Returns NULL on out-of-memory / too many
 * devices.  The device is not visible to userspace until drm_dev_register(). */
struct drm_device *drm_dev_alloc(const drm_driver_ops_t *ops, void *priv);
void               drm_dev_free(struct drm_device *dev);
void              *drm_dev_priv(struct drm_device *dev);
const drm_driver_ops_t *drm_dev_ops(struct drm_device *dev);

/* Bring the device up: call ops->load(), then publish /dev/dri/cardN (and the
 * matching renderDN node).  Returns 0 on success, negative on failure — a
 * driver that fails here must call drm_dev_free(). */
int  drm_dev_register(struct drm_device *dev);
void drm_dev_unregister(struct drm_device *dev);

/* Convenience for module probes: alloc + register in one step. */
struct drm_device *drm_dev_create(const drm_driver_ops_t *ops, void *priv);

/* --------------------------------------------------------- KMS object init */

/* All four return 0 on success, -EINVAL when the index is out of range, and
 * -ENOMEM when per-object state cannot be allocated.  The driver fills in the
 * returned object (modes, EDID, formats). */
int drm_mode_crtc_init(struct drm_device *dev, int index, const char *name);
int drm_mode_encoder_init(struct drm_device *dev, int index, uint32_t type,
                          uint32_t possible_crtcs, uint32_t possible_clones);
int drm_mode_connector_init(struct drm_device *dev, int index, uint32_t type,
                            uint32_t type_id);
int drm_mode_plane_init(struct drm_device *dev, int index, uint32_t type,
                        uint32_t possible_crtcs,
                        const uint32_t *formats, int format_count,
                        uint32_t format_type);

struct drm_crtc      *drm_crtc_find     (struct drm_device *dev, uint32_t id);
struct drm_connector *drm_connector_find(struct drm_device *dev, uint32_t id);
struct drm_encoder   *drm_encoder_find  (struct drm_device *dev, uint32_t id);
struct drm_plane     *drm_plane_find    (struct drm_device *dev, uint32_t id);

/* Connector helpers used by drivers while filling in a connector. */
void drm_connector_add_mode(struct drm_connector *conn,
                            const struct drm_display_mode *mode);
void drm_connector_set_edid(struct drm_connector *conn,
                            const void *edid, uint32_t len);
void drm_connector_attach_encoder(struct drm_connector *conn,
                                  struct drm_encoder *enc);
void drm_encoder_attach_crtc(struct drm_encoder *enc, struct drm_crtc *crtc);

/* Vblank reporting for drivers with a real interrupt.  A driver whose device
 * has no vblank interrupt leaves enable_vblank() NULL; the core then keeps the
 * CRTC's counter moving on every page flip / mode set, so DRM_IOCTL_WAIT_VBLANK
 * and DRM_MODE_PAGE_FLIP_EVENT still make progress. */
void drm_crtc_handle_vblank(struct drm_crtc *crtc);

/* ------------------------------------------------------------------- GEM */

/* Create an object of `size` bytes (rounded up to a page) backed by a memfd
 * object, and return it with one reference held.  NULL on failure. */
struct drm_gem_object *drm_gem_create(struct drm_device *dev, uint32_t size);
/* Kernel pointer to byte `off` of the object's storage (identity-mapped). */
void  *drm_gem_vaddr(struct drm_gem_object *obj, uint32_t off);
uint32_t drm_gem_size(struct drm_gem_object *obj);
int   drm_gem_ref(struct drm_gem_object *obj);
int   drm_gem_unref(struct drm_gem_object *obj);
/* memfd handle backing the object — used by PRIME and by drivers that need to
 * hand the storage to another subsystem. */
int   drm_gem_memfd(struct drm_gem_object *obj);

/* Offset to hand to mmap(2) for a GEM object (DRM_IOCTL_VIRTGPU_MAP and
 * friends).  One is allocated on first use; the core's mmap handler resolves
 * it back to the object. */
int   drm_gem_map_offset(struct drm_device *dev, struct drm_gem_object *obj,
                         uint64_t *offset_out);

/* Find/create a GEM object for an already-open memfd, and turn it into a
 * client handle.  This is the import half of PRIME. */
int drm_gem_handle_create(struct drm_file *file, struct drm_gem_object *obj,
                          uint32_t *handle_out);
struct drm_gem_object *drm_gem_handle_lookup(struct drm_file *file,
                                             uint32_t handle);
int  drm_gem_handle_close(struct drm_file *file, uint32_t handle);
int  drm_gem_prime_handle_to_fd(struct drm_device *dev, struct drm_file *file,
                                uint32_t handle, uint32_t flags, int *fd_out);
int  drm_gem_prime_fd_to_handle(struct drm_device *dev, struct drm_file *file,
                                int fd, uint32_t *handle_out);

/* --------------------------------------------------------------- logging */

#endif /* DRM_DRV_H */
