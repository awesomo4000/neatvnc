/*
 * Copyright (c) 2019 - 2025 Andri Yngvason
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES WITH
 * REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY
 * AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY SPECIAL, DIRECT,
 * INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM
 * LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE
 * OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR
 * PERFORMANCE OF THIS SOFTWARE.
 */

#include "neatvnc.h"
#include "rfb-proto.h"
#include "pixels.h"
#include "vec.h"
#include "config.h"
#include "enc/util.h"
#include "frame.h"
#include "enc/encoder.h"

#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <zlib.h>
#include <pixels.h>
#include <pthread.h>
#include <assert.h>
#include <aml.h>
#include <libdrm/drm_fourcc.h>
#ifdef HAVE_JPEG
#include <turbojpeg.h>
#endif

#define UDIV_UP(a, b) (((a) + (b) - 1) / (b))

#define TIGHT_FILL 0x80
#define TIGHT_JPEG 0x90
#define TIGHT_PNG 0xA0
#define TIGHT_BASIC 0x00

#define TIGHT_STREAM(n) ((n) << 4)
#define TIGHT_FILTER_ID 0x40
#define TIGHT_FILTER_COPY 0
#define TIGHT_FILTER_PALETTE 1

/* Below this many bytes the tight spec sends filtered data uncompressed and
 * without a length prefix. Tiles here are 64x64 and edge tiles on a 1280x800
 * screen are 64x32, so palette data never gets that small -- but fall back to
 * plain copy rather than emit something a client would misparse. */
#define TIGHT_MIN_TO_COMPRESS 12

/* Power of two, comfortably above the 256-colour cap so probing stays short. */
#define TIGHT_PAL_SLOTS 1024
#define TIGHT_RESET(n) (1 << (n))

#define TSL 64 /* Tile Side Length */

#define MAX_TILE_SIZE (2 * TSL * TSL * 4)

struct encoder* tight_encoder_new(uint16_t width, uint16_t height);

typedef void (*tight_done_fn)(struct vec* frame, void*);

struct tight_encoder_grid {
	struct tight_tile* grid;
	uint32_t width;
	uint32_t height;
};

struct tight_encoder {
	struct encoder encoder;

	int last_n_fbs;
	uint32_t width;
	uint32_t height;
	int quality;

	struct tight_encoder_grid grid[NVNC_FB_COMPOSITE_MAX];

	z_stream zs[4];
	struct aml_work* zs_worker[4];

	struct nvnc_pixel_format dfmt;

	uint8_t tile_buf[4][TSL * TSL * 4];

	/* Palette scratch, per zlib worker. The workers run concurrently on
	 * disjoint tile columns, so each needs its own. */
	uint8_t pack_buf[4][TSL * TSL];
	uint32_t pal_key[4][TIGHT_PAL_SLOTS];
	int16_t pal_idx[4][TIGHT_PAL_SLOTS];
	struct nvnc_composite_fb composite_fb;

	uint64_t pts;

	uint32_t n_rects;
	uint32_t n_jobs;

	struct vec dst;

	tight_done_fn on_frame_done;
	void* userdata;
};

enum tight_tile_state {
	TIGHT_TILE_READY = 0,
	TIGHT_TILE_DAMAGED,
	TIGHT_TILE_ENCODED,
};

struct tight_tile {
	enum tight_tile_state state;
	size_t size;
	/* Bytes at the start of buffer that are written verbatim and excluded
	 * from the compressed-length prefix: the filter id and the palette. */
	size_t hdr_size;
	uint8_t type;
	char buffer[MAX_TILE_SIZE];
};

struct tight_zs_worker_ctx {
	struct tight_encoder* encoder;
	int index;
};

struct encoder_impl encoder_impl_tight;

static void do_tight_zs_work(struct aml_work*);
static void on_tight_zs_work_done(struct aml_work*);
static int schedule_tight_finish(struct tight_encoder* self);

static inline struct tight_encoder* tight_encoder(struct encoder* encoder)
{
	assert(encoder->impl == &encoder_impl_tight);
	return (struct tight_encoder*)encoder;
}

static int tight_encoder_init_stream(z_stream* zs)
{
	int rc = deflateInit2(zs,
			/* compression level: */ 1,
			/*            method: */ Z_DEFLATED,
			/*       window bits: */ 15,
			/*         mem level: */ 9,
			/*          strategy: */ Z_DEFAULT_STRATEGY);
	return rc == Z_OK ? 0 : -1;
}

static inline struct tight_tile* tight_tile(struct tight_encoder* self,
		int fb_index, uint32_t x, uint32_t y)
{
	struct tight_encoder_grid* grid = &self->grid[fb_index];
	return &grid->grid[x + y * grid->width];
}

static inline uint32_t tight_tile_width(struct tight_encoder* self,
		int fb_index, uint32_t x)
{
	uint32_t width = self->composite_fb.fbs[fb_index]->width;
	return x + TSL > width ? width - x : TSL;
}

static inline uint32_t tight_tile_height(struct tight_encoder* self,
		int fb_index, uint32_t y)
{
	uint32_t height = self->composite_fb.fbs[fb_index]->height;
	return y + TSL > height ? height - y : TSL;
}

static int tight_init_zs_worker(struct tight_encoder* self, int index)
{
	struct tight_zs_worker_ctx* ctx = calloc(1, sizeof(*ctx));
	if (!ctx)
		return -1;

	ctx->encoder = self;
	ctx->index = index;

	self->zs_worker[index] =
		aml_work_new(do_tight_zs_work, on_tight_zs_work_done, ctx, free);
	if (!self->zs_worker[index])
		goto failure;

	return 0;

failure:
	free(ctx);
	return -1;
}

static void tight_encoder_resize(struct tight_encoder* self)
{
	struct nvnc_composite_fb *cfb = &self->composite_fb;
	uint16_t width, height;
	width = nvnc_composite_fb_width(cfb);
	height = nvnc_composite_fb_height(cfb);
	int n_fbs = cfb->n_fbs;

	if (self->width == width && self->height == height &&
			self->last_n_fbs == n_fbs)
		return;

	self->width = width;
	self->height = height;
	self->last_n_fbs = n_fbs;

	for (int i = 0; i < NVNC_FB_COMPOSITE_MAX && self->grid[i].grid; ++i) {
		free(self->grid[i].grid);
		self->grid[i].grid = NULL;
	}

	for (int i = 0; i < self->composite_fb.n_fbs; ++i) {
		struct nvnc_frame* fb = self->composite_fb.fbs[i];
		assert(fb);

		struct tight_encoder_grid *grid = &self->grid[i];

		grid->width = UDIV_UP(fb->width, TSL);
		grid->height = UDIV_UP(fb->height, TSL);

		grid->grid = calloc(grid->width * grid->height,
				sizeof(*grid->grid));
		nvnc_assert(grid->grid, "OOM");
	}
}

static int tight_encoder_init(struct tight_encoder* self, uint32_t width,
		uint32_t height)
{
	memset(self, 0, sizeof(*self));

	tight_encoder_init_stream(&self->zs[0]);
	tight_encoder_init_stream(&self->zs[1]);
	tight_encoder_init_stream(&self->zs[2]);
	tight_encoder_init_stream(&self->zs[3]);

	tight_init_zs_worker(self, 0);
	tight_init_zs_worker(self, 1);
	tight_init_zs_worker(self, 2);
	tight_init_zs_worker(self, 3);

	aml_require_workers(aml_get_default(), 1);

	self->pts = NVNC_NO_PTS;

	return 0;
}

static void tight_encoder_destroy(struct tight_encoder* self)
{
	aml_unref(self->zs_worker[3]);
	aml_unref(self->zs_worker[2]);
	aml_unref(self->zs_worker[1]);
	aml_unref(self->zs_worker[0]);

	deflateEnd(&self->zs[3]);
	deflateEnd(&self->zs[2]);
	deflateEnd(&self->zs[1]);
	deflateEnd(&self->zs[0]);

	for (int i = 0; i < NVNC_FB_COMPOSITE_MAX && self->grid[i].grid; ++i)
		free(self->grid[i].grid);
}

static int tight_apply_damage(struct tight_encoder* self,
		struct pixman_region16* damage)
{
	int n_damaged = 0;

	for (int fbi = 0; fbi < self->composite_fb.n_fbs; ++fbi) {
		struct nvnc_frame* fb = self->composite_fb.fbs[fbi];
		struct tight_encoder_grid *grid = &self->grid[fbi];

		for (uint32_t y = 0; y < grid->height; ++y) {
			for (uint32_t x = 0; x < grid->width; ++x) {
				struct pixman_box16 box = {
					.x1 = fb->x_off + x * TSL,
					.y1 = fb->y_off + y * TSL,
				};
				box.x2 = box.x1 + tight_tile_width(self, fbi, x);
				box.y2 = box.y1 + tight_tile_height(self, fbi, y);

				pixman_region_overlap_t overlap
					= pixman_region_contains_rectangle(damage, &box);

				if (overlap != PIXMAN_REGION_OUT) {
					++n_damaged;
					tight_tile(self, fbi, x, y)->state =
						TIGHT_TILE_DAMAGED;
				} else {
					tight_tile(self, fbi, x, y)->state =
						TIGHT_TILE_READY;
				}
			}
		}
	}

	return n_damaged;
}

static void tight_encode_size(struct vec* dst, size_t size)
{
	vec_fast_append_8(dst, (size & 0x7f) | ((size >= 128) << 7));
	if (size >= 128)
		vec_fast_append_8(dst, ((size >> 7) & 0x7f) | ((size >= 16384) << 7));
	if (size >= 16384)
		vec_fast_append_8(dst, (size >> 14) & 0xff);
}

static int tight_deflate(struct tight_tile* tile, void* src,
		size_t len, z_stream* zs, bool flush)
{
	zs->next_in = src;
	zs->avail_in = len;

	do {
		if (tile->size >= MAX_TILE_SIZE)
			return -1;

		zs->next_out = ((Bytef*)tile->buffer) + tile->size;
		zs->avail_out = MAX_TILE_SIZE - tile->size;

		int r = deflate(zs, flush ? Z_SYNC_FLUSH : Z_NO_FLUSH);
		if (r == Z_STREAM_ERROR)
			return -1;

		tile->size = zs->next_out - (Bytef*)tile->buffer;
	} while (zs->avail_out == 0);

	assert(zs->avail_in == 0);

	return 0;
}


static inline uint32_t tight_px_key(const uint8_t* p, int bpp)
{
	uint32_t v = 0;
	for (int i = 0; i < bpp; ++i)
		v |= (uint32_t)p[i] << (8 * i);
	return v;
}

static inline uint32_t tight_px_slot(uint32_t key)
{
	/* Knuth multiplicative; the low bits of a pixel vary least, so mix
	 * before masking. */
	return (key * 2654435761u) >> (32 - 10);
}

/* Collect distinct colours into palette[], returning the count, or 257 if the
 * tile exceeds 256 colours (in which case the caller falls back to copy).
 * key/idx are caller-provided scratch so this allocates nothing. */
static int tight_build_palette(const uint8_t* buf, size_t n_px, int bpp,
		uint8_t* palette, uint32_t* key, int16_t* idx)
{
	memset(idx, 0xff, TIGHT_PAL_SLOTS * sizeof(*idx));
	int n = 0;

	for (size_t i = 0; i < n_px; ++i) {
		const uint8_t* px = buf + i * bpp;
		uint32_t k = tight_px_key(px, bpp);
		uint32_t s = tight_px_slot(k);

		for (;;) {
			if (idx[s] < 0) {
				if (n == 256)
					return 257;
				key[s] = k;
				idx[s] = n;
				memcpy(palette + (size_t)n * bpp, px, bpp);
				++n;
				break;
			}
			if (key[s] == k)
				break;
			s = (s + 1) & (TIGHT_PAL_SLOTS - 1);
		}
	}

	return n;
}

/* Two colours: one bit per pixel, rows padded to a byte boundary. Bit set
 * means palette entry 1. */
static void tight_pack_mono(const uint8_t* buf, uint8_t* out, uint32_t width,
		uint32_t height, int bpp, const uint8_t* palette)
{
	size_t stride = (width + 7) / 8;
	memset(out, 0, stride * height);

	for (uint32_t y = 0; y < height; ++y) {
		const uint8_t* row = buf + (size_t)y * width * bpp;
		uint8_t* orow = out + (size_t)y * stride;
		for (uint32_t x = 0; x < width; ++x)
			if (memcmp(row + (size_t)x * bpp, palette + bpp, bpp) == 0)
				orow[x >> 3] |= 0x80 >> (x & 7);
	}
}

/* Three to 256 colours: one byte per pixel. */
static void tight_pack_indexed(const uint8_t* buf, uint8_t* out, size_t n_px,
		int bpp, const uint32_t* key, const int16_t* idx)
{
	for (size_t i = 0; i < n_px; ++i) {
		uint32_t k = tight_px_key(buf + i * bpp, bpp);
		uint32_t s = tight_px_slot(k);
		while (key[s] != k)
			s = (s + 1) & (TIGHT_PAL_SLOTS - 1);
		out[i] = (uint8_t)idx[s];
	}
}

static void tight_encode_tile_basic(struct tight_encoder* self,
		struct tight_tile* tile, int fb_index, uint32_t x,
		uint32_t y, uint32_t width, uint32_t height, int zs_index)
{
	z_stream* zs = &self->zs[zs_index];
	tile->type = TIGHT_BASIC | TIGHT_STREAM(zs_index);

	struct nvnc_pixel_format cfmt;
	if (self->dfmt.bytes_per_pixel == 4 && self->dfmt.red_size == 8 &&
			self->dfmt.green_size == 8 &&
			self->dfmt.blue_size == 8)
		nvnc_pixel_format_from_fourcc(&cfmt, DRM_FORMAT_BGR888);
	else
		cfmt = self->dfmt;

	int bytes_per_cpixel = cfmt.bytes_per_pixel;
	assert(bytes_per_cpixel <= 4);

	struct nvnc_frame* fb = self->composite_fb.fbs[fb_index];
	uint8_t* buf = self->tile_buf[zs_index];

	// TODO: Limit width and height to the sides
	struct nvnc_frame_copy_options options = {
		.crop = { x, y, width, height },
		.buffer = buf,
		.stride = width,
		.format = &cfmt,
	};
	nvnc_frame_copy_region(fb, &options);

	/* Solid tiles become TIGHT_FILL: one TPIXEL instead of a deflated
	 * copy of the whole tile.
	 *
	 * This matters far more than it looks. neatvnc previously emitted
	 * basic:copy for every tile without exception -- 24-bit pixels pushed
	 * through zlib -- while TigerVNC picks a sub-encoding per tile.
	 * Measured on the wire for the same screen: neatvnc 260 tiles /
	 * 92,710 bytes, all basic:copy, against TigerVNC 5,922 bytes. A
	 * desktop is mostly uniform background, so most tiles are solid and
	 * cost 4 bytes here instead of a deflate stream.
	 *
	 * Compare in the converted pixel format rather than the source, so
	 * the comparison matches exactly what would otherwise be sent. */
	size_t n_px = (size_t)width * height;
	bool is_solid = n_px > 0;
	for (size_t i = 1; i < n_px && is_solid; ++i)
		is_solid = memcmp(buf, buf + i * bytes_per_cpixel,
				bytes_per_cpixel) == 0;

	if (is_solid) {
		tile->type = TIGHT_FILL;
		tile->size = bytes_per_cpixel;
		tile->hdr_size = 0;
		memcpy(tile->buffer, buf, bytes_per_cpixel);
		return;
	}

	/* Palette: the encoding that actually matters for terminal text.
	 *
	 * Two colours pack to 1 bit per pixel and anything up to 256 packs to
	 * 8, against the 24 that basic:copy sends. zlib helps either way, but
	 * it cannot recover the factor of 24 that is thrown away by sending
	 * full pixels in the first place. TigerVNC does this and neatvnc did
	 * not, which is most of why the same screen cost 92,710 bytes here
	 * against 5,922 there.
	 *
	 * Colours are collected through an open-addressed table rather than a
	 * linear scan: a linear scan is O(pixels * colours), which at 4096
	 * pixels and 256 colours is a million comparisons per tile. */
	uint8_t palette[256 * 4];
	int n_colors = tight_build_palette(buf, n_px, bytes_per_cpixel,
			palette, self->pal_key[zs_index],
			self->pal_idx[zs_index]);

	if (n_colors >= 2 && n_colors <= 256) {
		size_t data_len = (n_colors == 2)
			? (size_t)((width + 7) / 8) * height
			: n_px;

		if (data_len >= TIGHT_MIN_TO_COMPRESS) {
			uint8_t* packed = self->pack_buf[zs_index];
			if (n_colors == 2)
				tight_pack_mono(buf, packed, width, height,
						bytes_per_cpixel, palette);
			else
				tight_pack_indexed(buf, packed, n_px,
						bytes_per_cpixel,
						self->pal_key[zs_index],
						self->pal_idx[zs_index]);

			tile->type = TIGHT_BASIC | TIGHT_STREAM(zs_index) |
					TIGHT_FILTER_ID;
			tile->buffer[0] = TIGHT_FILTER_PALETTE;
			tile->buffer[1] = (uint8_t)(n_colors - 1);
			memcpy(tile->buffer + 2, palette,
					(size_t)n_colors * bytes_per_cpixel);
			tile->hdr_size = 2 + (size_t)n_colors * bytes_per_cpixel;
			tile->size = tile->hdr_size;

			if (tight_deflate(tile, packed, data_len, zs, true) < 0)
				abort();
			return;
		}
	}

	tile->hdr_size = 0;

	// TODO: What to do if the buffer fills up?
	if (tight_deflate(tile, buf, bytes_per_cpixel * width * height,
			zs, true) < 0)
		abort();
}

#ifdef HAVE_JPEG
static enum TJPF tight_get_jpeg_pixfmt(uint32_t fourcc)
{
	switch (fourcc) {
	case DRM_FORMAT_RGBA8888:
	case DRM_FORMAT_RGBX8888:
		return TJPF_XBGR;
	case DRM_FORMAT_BGRA8888:
	case DRM_FORMAT_BGRX8888:
		return TJPF_XRGB;
	case DRM_FORMAT_ARGB8888:
	case DRM_FORMAT_XRGB8888:
		return TJPF_BGRX;
	case DRM_FORMAT_ABGR8888:
	case DRM_FORMAT_XBGR8888:
		return TJPF_RGBX;
	case DRM_FORMAT_BGR888:
		return TJPF_RGB;
	case DRM_FORMAT_RGB888:
		return TJPF_BGR;
	}

	return TJPF_UNKNOWN;
}

static int tight_encode_tile_jpeg(struct tight_encoder* self,
		struct tight_tile* tile, int fb_index, uint32_t x, uint32_t y,
		uint32_t width, uint32_t height)
{
	tile->type = TIGHT_JPEG;

	unsigned char* buffer = NULL;
	unsigned long size = 0;

	int quality = 11 * self->quality + 1;

	struct nvnc_frame* fb = self->composite_fb.fbs[fb_index];
	uint32_t fourcc = nvnc_frame_get_fourcc_format(fb);
	enum TJPF tjfmt = tight_get_jpeg_pixfmt(fourcc);
	if (tjfmt == TJPF_UNKNOWN)
		return -1;

	tjhandle handle = tjInitCompress();
	if (!handle)
		return -1;

	uint8_t* addr = nvnc_frame_get_addr(fb);
	int32_t bpp = nvnc__pixel_size_from_fourcc(fourcc);
	int32_t byte_stride = nvnc_frame_get_stride(fb) * bpp;
	int32_t xoff = x * bpp;
	uint8_t* img = addr + xoff + y * byte_stride;

	enum TJSAMP subsampling = (self->quality == 9) ? TJSAMP_444 : TJSAMP_420;

	int rc = -1;
	rc = tjCompress2(handle, img, width, byte_stride, height, tjfmt, &buffer,
			&size, subsampling, quality, TJFLAG_FASTDCT);
	if (rc < 0) {
		nvnc_log(NVNC_LOG_ERROR, "Failed to encode tight JPEG box: %s",
				tjGetErrorStr());
		goto failure;
	}

	if (size > MAX_TILE_SIZE) {
		nvnc_log(NVNC_LOG_ERROR, "Whoops, encoded JPEG was too big for the buffer");
		goto failure;
	}

	memcpy(tile->buffer, buffer, size);
	tile->size = size;

	rc = 0;
	tjFree(buffer);
failure:
	tjDestroy(handle);

	return rc;
}
#endif /* HAVE_JPEG */

static void tight_encode_tile(struct tight_encoder* self, int fb_index,
		uint32_t gx, uint32_t gy)
{
	struct tight_tile* tile = tight_tile(self, fb_index, gx, gy);

	uint32_t x = gx * TSL;
	uint32_t y = gy * TSL;

	uint32_t width = tight_tile_width(self, fb_index, x);
	uint32_t height = tight_tile_height(self, fb_index, y);

	tile->size = 0;

#ifdef HAVE_JPEG
	if (self->quality >= 10) {
		tight_encode_tile_basic(self, tile, fb_index, x, y, width,
				height, gx % 4);
	} else {
		tight_encode_tile_jpeg(self, tile, fb_index, x, y, width,
				height);
	}
#else
	tight_encode_tile_basic(self, tile, fb_index, x, y, width, height, gx % 4);
#endif

	tile->state = TIGHT_TILE_ENCODED;
}

static void do_tight_zs_work(struct aml_work* work)
{
	struct tight_zs_worker_ctx* ctx = aml_get_userdata(work);
	struct tight_encoder* self = ctx->encoder;
	int index = ctx->index;

	for (int fbi = 0; fbi < self->composite_fb.n_fbs; ++fbi)
		for (uint32_t y = 0; y < self->grid[fbi].height; ++y)
			for (uint32_t x = index; x < self->grid[fbi].width; x += 4)
				if (tight_tile(self, fbi, x, y)->state == TIGHT_TILE_DAMAGED)
					tight_encode_tile(self, fbi, x, y);
}

static void on_tight_zs_work_done(struct aml_work* obj)
{
	struct tight_zs_worker_ctx* ctx = aml_get_userdata(obj);
	struct tight_encoder* self = ctx->encoder;

	if (--self->n_jobs == 0) {
		schedule_tight_finish(self);
	}

	encoder_unref(&self->encoder);
}

static int tight_schedule_zs_work(struct tight_encoder* self, int index)
{
	encoder_ref(&self->encoder);

	int rc = aml_start(aml_get_default(), self->zs_worker[index]);
	if (rc >= 0)
		++self->n_jobs;
	else
		encoder_unref(&self->encoder);

	return rc;
}

static int tight_schedule_encoding_jobs(struct tight_encoder* self)
{
	for (int i = 0; i < 4; ++i)
		if (tight_schedule_zs_work(self, i) < 0)
			return -1;

	return 0;
}

static void tight_finish_tile(struct tight_encoder* self,
		int fb_index, uint32_t gx, uint32_t gy)
{
	struct tight_tile* tile = tight_tile(self, fb_index, gx, gy);

	struct nvnc_frame* fb = self->composite_fb.fbs[fb_index];
	uint16_t x_pos = fb->x_off;
	uint16_t y_pos = fb->y_off;

	uint32_t x = gx * TSL;
	uint32_t y = gy * TSL;

	uint32_t width = tight_tile_width(self, fb_index, x);
	uint32_t height = tight_tile_height(self, fb_index, y);

	nvnc__encode_rect_head(&self->dst, RFB_ENCODING_TIGHT, x_pos + x, y_pos + y,
			width, height);

	vec_append(&self->dst, &tile->type, sizeof(tile->type));

	if (tile->type == TIGHT_FILL) {
		/* A bare TPIXEL: no length prefix, nothing deflated. */
		vec_append(&self->dst, tile->buffer, tile->size);
	} else {
		/* Any filter id and palette go out verbatim ahead of the
		 * length, which covers only the compressed remainder. */
		if (tile->hdr_size)
			vec_append(&self->dst, tile->buffer, tile->hdr_size);
		tight_encode_size(&self->dst, tile->size - tile->hdr_size);
		vec_append(&self->dst, tile->buffer + tile->hdr_size,
				tile->size - tile->hdr_size);
	}

	tile->state = TIGHT_TILE_READY;
}

static void tight_finish(struct tight_encoder* self)
{
	for (int fbi = 0; fbi < self->composite_fb.n_fbs; ++fbi)
		for (uint32_t y = 0; y < self->grid[fbi].height; ++y)
			for (uint32_t x = 0; x < self->grid[fbi].width; ++x)
				if (tight_tile(self, fbi, x, y)->state ==
						TIGHT_TILE_ENCODED)
					tight_finish_tile(self, fbi, x, y);
}

static void do_tight_finish(struct aml_work* work)
{
	struct tight_encoder* self = aml_get_userdata(work);
	tight_finish(self);
}

static void on_tight_finished(struct aml_work* work)
{
	struct tight_encoder* self = aml_get_userdata(work);

	struct nvnc_frame_metadata* metadata = self->composite_fb.metadata;
	if (metadata)
		nvnc_frame_metadata_ref(metadata);

	nvnc_composite_fb_unref(&self->composite_fb);
	memset(&self->composite_fb, 0, sizeof(self->composite_fb));

	struct encoded_frame* result;
	result = nvnc__encoded_frame_new(self->dst.data, self->dst.len,
			self->n_rects, self->width, self->height, self->pts);
	assert(result);

	result->metadata = metadata;

	encoder_finish_frame(&self->encoder, result);

	self->pts = NVNC_NO_PTS;
	nvnc_frame_metadata_unref(metadata);
	encoded_frame_unref(result);
	encoder_unref(&self->encoder);
}

static int schedule_tight_finish(struct tight_encoder* self)
{
	encoder_ref(&self->encoder);

	struct aml_work* work = aml_work_new(do_tight_finish, on_tight_finished,
			self, NULL);
	if (!work) {
		encoder_unref(&self->encoder);
		return -1;
	}

	int rc = aml_start(aml_get_default(), work);
	aml_unref(work);
	return rc;
}

struct encoder* tight_encoder_new(uint16_t width, uint16_t height)
{
	struct tight_encoder* self = calloc(1, sizeof(*self));
	if (!self)
		return NULL;

	if (tight_encoder_init(self, width, height) < 0) {
		free(self);
		return NULL;
	}

	encoder_init(&self->encoder, &encoder_impl_tight);

	return (struct encoder*)self;
}

static void tight_encoder_destroy_wrapper(struct encoder* encoder)
{
	tight_encoder_destroy(tight_encoder(encoder));
	free(encoder);
}

static void tight_encoder_set_output_format(struct encoder* encoder,
		const struct nvnc_pixel_format* pixfmt)
{
	struct tight_encoder* self = tight_encoder(encoder);
	memcpy(&self->dfmt, pixfmt, sizeof(self->dfmt));
}

static void tight_encoder_set_quality(struct encoder* encoder, int value)
{
	struct tight_encoder* self = tight_encoder(encoder);
	self->quality = value;
}

static int tight_encoder_encode(struct encoder* encoder,
		struct nvnc_composite_fb* composite_fb,
		struct pixman_region16* damage)
{
	struct tight_encoder* self = tight_encoder(encoder);
	int rc;

	nvnc_composite_fb_copy(&self->composite_fb, composite_fb);
	self->pts = nvnc_composite_fb_pts(composite_fb);

	tight_encoder_resize(self);

	rc = nvnc_composite_fb_map(composite_fb);
	nvnc_assert(rc == 0, "Failed to map input buffer");

	// TODO: Estimate a better buffer size
	rc = vec_init(&self->dst, self->width * self->height * 4);
	if (rc < 0)
		return -1;

	self->n_rects = tight_apply_damage(self, damage);
	assert(self->n_rects > 0);

	rc = tight_schedule_encoding_jobs(self);
	nvnc_assert(rc == 0, "Failed to schedule encoding jobs");

	return 0;
}

struct encoder_impl encoder_impl_tight = {
	.destroy = tight_encoder_destroy_wrapper,
	.set_output_format = tight_encoder_set_output_format,
	.set_quality = tight_encoder_set_quality,
	.encode = tight_encoder_encode,
};
