/* See LICENSE for licence details. */
#include <linux/vt.h>
#include <linux/kd.h>

#include <xf86drm.h>
#include <xf86drmMode.h>

static const char *dri_path = "/dev/dri/card0";

enum {
	DEPTH = 24,
	BPP = 32,
};

/* struct for DRM */
struct drm_dev_t {
	char *buf;
	uint32_t conn_id, enc_id, crtc_id;
	drmModeModeInfo mode;
	drmModeCrtc *saved_crtc;

	uint32_t width, height;
	uint32_t pitch, size, handle;
	uint32_t fb_id;

	struct drm_dev_t *next;
};

struct framebuffer {
	char *copy;                       /* copy of framebuffer */
	char *wall;                       /* buffer for wallpaper */
	int fd;                           /* file descriptor of framebuffer */
	int width, height;                /* framebuffer resolution */
	long screen_size;                 /* screen data size (byte) */
	int line_length;                  /* line length (byte) */
	int bpp;                          /* BYTES per pixel */
	struct drm_dev_t *drm_head, *drm; /* for drm */
};

#include "util.h"

/* functions for DRM */
int drm_open(const char *path)
{
	int fd, flags;
	uint64_t has_dumb;

	fd = eopen(path, O_RDWR);

	/* set FD_CLOEXEC flag */
	if ((flags = fcntl(fd, F_GETFD)) < 0
		|| fcntl(fd, F_SETFD, flags | FD_CLOEXEC) < 0)
		fatal("fcntl FD_CLOEXEC failed");

	/* check capability */
	if (drmGetCap(fd, DRM_CAP_DUMB_BUFFER, &has_dumb) < 0 || has_dumb == 0)
		fatal("drmGetCap DRM_CAP_DUMB_BUFFER failed or doesn't have dumb buffer");

	return fd;
}

struct drm_dev_t *drm_find_dev(int fd)
{
	int i;
	struct drm_dev_t *dev = NULL, *dev_head = NULL;
	drmModeRes *res;
	drmModeConnector *conn;
	drmModeEncoder *enc;

	if ((res = drmModeGetResources(fd)) == NULL)
		fatal("drmModeGetResources() failed");

	/* find all available connectors */
	for (i = 0; i < res->count_connectors; i++) {
		conn = drmModeGetConnector(fd, res->connectors[i]);

		if (conn != NULL && conn->connection == DRM_MODE_CONNECTED && conn->count_modes > 0) {
			dev = (struct drm_dev_t *) emalloc(sizeof(struct drm_dev_t));
			memset(dev, 0, sizeof(struct drm_dev_t));

			/* set required info */
			dev->conn_id = conn->connector_id;
			dev->enc_id = conn->encoder_id;

			memcpy(&dev->mode, &conn->modes[0], sizeof(drmModeModeInfo));
			dev->width = conn->modes[0].hdisplay;
			dev->height = conn->modes[0].vdisplay;

			/* FIXME: use default encoder/crtc pair */
			if ((enc = drmModeGetEncoder(fd, dev->enc_id)) == NULL)
				fatal("drmModeGetEncoder() faild");
			dev->crtc_id = enc->crtc_id;
			drmModeFreeEncoder(enc);

			dev->saved_crtc = NULL;
			dev->next = NULL;

			/* create dev list */
			dev->next = dev_head;
			dev_head = dev;
		}
		drmModeFreeConnector(conn);
	}
	drmModeFreeResources(res);

	return dev_head;
}

void drm_setup_fb(int fd, struct drm_dev_t *dev)
{
	struct drm_mode_create_dumb creq;
	struct drm_mode_map_dumb mreq;

	/* create dumb buffer */
	memset(&creq, 0, sizeof(struct drm_mode_create_dumb));
	creq.width = dev->width;
	creq.height = dev->height;
	creq.bpp = BPP; // hard conding

	if (drmIoctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &creq) < 0)
		fatal("drmIoctl DRM_IOCTL_MODE_CREATE_DUMB failed");

	dev->pitch = creq.pitch;
	dev->size = creq.size;
	dev->handle = creq.handle;

	/* create framebuffer */
	if (drmModeAddFB(fd, dev->width, dev->height,
		DEPTH, BPP, dev->pitch, dev->handle, &dev->fb_id))
		fatal("drmModeAddFB failed");

	memset(&mreq, 0, sizeof(struct drm_mode_map_dumb));
	mreq.handle = dev->handle;

	/* mmap */
	if (drmIoctl(fd, DRM_IOCTL_MODE_MAP_DUMB, &mreq))
		fatal("drmIoctl DRM_IOCTL_MODE_MAP_DUMB failed");

	dev->buf = (char *) emmap(0, dev->size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, mreq.offset);

	/* set crtc mode */
	dev->saved_crtc = drmModeGetCrtc(fd, dev->crtc_id); /* must store crtc data */
	if (drmModeSetCrtc(fd, dev->crtc_id, dev->fb_id, 0, 0, &dev->conn_id, 1, &dev->mode))
		fatal("drmModeSetCrtc() failed");
}

void fb_die(struct framebuffer *fb)
{
	struct drm_dev_t *devp, *devp_tmp;
	struct drm_mode_destroy_dumb dreq;

	for (devp = fb->drm_head; devp != NULL;) {
		if (devp->saved_crtc) /* restore ctrtc setting */
			drmModeSetCrtc(fb->fd, devp->saved_crtc->crtc_id, devp->saved_crtc->buffer_id,
				devp->saved_crtc->x, devp->saved_crtc->y, &devp->conn_id, 1, &devp->saved_crtc->mode);

		/* free crtc/mmap/framebuffer/dumb buffer */
		drmModeFreeCrtc(devp->saved_crtc);

		munmap(devp->buf, devp->size);

		drmModeRmFB(fb->fd, devp->fb_id);

		memset(&dreq, 0, sizeof(dreq));
		dreq.handle = devp->handle;
		drmIoctl(fb->fd, DRM_IOCTL_MODE_DESTROY_DUMB, &dreq);

		devp_tmp = devp;
		devp = devp->next;
		free(devp_tmp);
	}

	close(fb->fd);
}

void fb_init(struct framebuffer *fb, uint32_t *color_palette)
{
	int i;
	struct drm_dev_t *dev;

	/* init */
	fb->fd = drm_open(dri_path);
	if ((fb->drm_head = drm_find_dev(fb->fd)) == NULL)
		fatal("available drm_dev not found\n");

	/* FIXME: use first drm_dev */
	dev = fb->drm_head;
	fb->width = dev->width;
	fb->height = dev->height;
	fb->bpp = BPP / BITS_PER_BYTE;

	/* init color palette */
	for (i = 0; i < COLORS; i++)
		color_palette[i] = color_list[i];

	/* set up framebuffer */
	drm_setup_fb(fb->fd, dev);

	fb->line_length = dev->pitch;
	fb->screen_size = dev->size;

	fb->wall = NULL;
	fb->copy = (char *) emalloc(fb->screen_size);
	fb->drm = fb->drm_head;
}

void draw_line(struct framebuffer *fb, struct terminal *term, int line)
{
	int pos, size, bit_shift, margin_right;
	int col, glyph_width_offset, glyph_height_offset;
	uint32_t pixel;
	struct color_pair color;
	struct cell *cp;
	const struct static_glyph_t *gp;

	for (col = term->cols - 1; col >= 0; col--) {
		margin_right = (term->cols - 1 - col) * CELL_WIDTH;

		/* get cell color and glyph */
		cp = &term->cells[col + line * term->cols];
		color = cp->color;
		gp = cp->gp;

		/* check cursor positon */
		if ((term->mode & MODE_CURSOR && line == term->cursor.y)
			&& (col == term->cursor.x
			|| (cp->width == WIDE && (col + 1) == term->cursor.x)
			|| (cp->width == NEXT_TO_WIDE && (col - 1) == term->cursor.x))) {
			color.fg = DEFAULT_BG;
			color.bg = (!tty.visible && tty.background_draw) ? PASSIVE_CURSOR_COLOR: ACTIVE_CURSOR_COLOR;
		}

		for (glyph_height_offset = 0; glyph_height_offset < CELL_HEIGHT; glyph_height_offset++) {
			if ((glyph_height_offset == (CELL_HEIGHT - 1)) && (cp->attribute & attr_mask[UNDERLINE]))
				color.bg = color.fg;

			for (glyph_width_offset = 0; glyph_width_offset < CELL_WIDTH; glyph_width_offset++) {
				pos = (term->width - 1 - margin_right - glyph_width_offset) * fb->bpp
					+ (line * CELL_HEIGHT + glyph_height_offset) * fb->line_length;

				if (cp->width == WIDE)
					bit_shift = glyph_width_offset + CELL_WIDTH;
				else
					bit_shift = glyph_width_offset;

				/* set color palette */
				if (gp->bitmap[glyph_height_offset] & (0x01 << bit_shift))
					pixel = term->color_palette[color.fg];
				else if (fb->wall && color.bg == DEFAULT_BG) /* wallpaper */
					memcpy(&pixel, fb->wall + pos, fb->bpp);
				else
					pixel = term->color_palette[color.bg];

				memcpy(fb->copy + pos, &pixel, fb->bpp);
			}
		}
	}

	pos = (line * CELL_HEIGHT) * fb->line_length;
	size = CELL_HEIGHT * fb->line_length;

	memcpy(fb->drm->buf + pos, fb->copy + pos, size);
	drmModeDirtyFB(fb->fd, fb->drm->fb_id, &(drmModeClip)
		{.x1 = 0, .y1 = line * CELL_HEIGHT, .x2 = fb->width - 1, .y2 = line * (CELL_HEIGHT + 1) - 1}, 1);

	term->line_dirty[line] = ((term->mode & MODE_CURSOR) && term->cursor.y == line) ? true: false;
}

void refresh(struct framebuffer *fb, struct terminal *term)
{
	int line;

	if (term->mode & MODE_CURSOR)
		term->line_dirty[term->cursor.y] = true;

	for (line = 0; line < term->lines; line++) {
		if (term->line_dirty[line])
			draw_line(fb, term, line);
	}

	if (tty.redraw_flag) {
		drmModeSetCrtc(fb->fd, fb->drm->crtc_id, fb->drm->fb_id,
			0, 0, &fb->drm->conn_id, 1, &fb->drm->mode);
		drmModeDirtyFB(fb->fd, fb->drm->fb_id, NULL, 0);
	}
}
