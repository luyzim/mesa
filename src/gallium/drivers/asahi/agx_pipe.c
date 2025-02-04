/*
 * Copyright 2010 Red Hat Inc.
 * Copyright © 2014-2017 Broadcom
 * Copyright (C) 2019-2020 Collabora, Ltd.
 * Copyright 2006 VMware, Inc.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * on the rights to use, copy, modify, merge, publish, distribute, sub
 * license, and/or sell copies of the Software, and to permit persons to whom
 * the Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHOR(S) AND/OR THEIR SUPPLIERS BE LIABLE FOR ANY CLAIM,
 * DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR
 * OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE
 * USE OR OTHER DEALINGS IN THE SOFTWARE.
 */
#include <stdio.h>
#include <errno.h>
#include "asahi/layout/layout.h"
#include "pipe/p_defines.h"
#include "pipe/p_state.h"
#include "pipe/p_context.h"
#include "pipe/p_screen.h"
#include "util/u_memory.h"
#include "util/u_screen.h"
#include "util/u_inlines.h"
#include "util/format/u_format.h"
#include "util/u_upload_mgr.h"
#include "util/half_float.h"
#include "frontend/winsys_handle.h"
#include "frontend/sw_winsys.h"
#include "gallium/auxiliary/util/u_transfer.h"
#include "gallium/auxiliary/util/u_transfer_helper.h"
#include "gallium/auxiliary/util/u_surface.h"
#include "gallium/auxiliary/util/u_framebuffer.h"
#include "gallium/auxiliary/util/u_debug_cb.h"
#include "gallium/auxiliary/renderonly/renderonly.h"
#include "agx_device.h"
#include "agx_public.h"
#include "agx_state.h"
#include "magic.h"
#include "asahi/compiler/agx_compile.h"
#include "asahi/lib/decode.h"
#include "asahi/lib/agx_formats.h"
#include "util/u_drm.h"

/* drm_fourcc cannot be built on macOS */
#ifndef __APPLE__
#include "drm-uapi/drm_fourcc.h"
#endif

/* In case of macOS, pick some fake modifier values so we still build */
#ifndef DRM_FORMAT_MOD_LINEAR
#define DRM_FORMAT_MOD_LINEAR 1
#endif

#ifndef DRM_FORMAT_MOD_APPLE_TWIDDLED
#define DRM_FORMAT_MOD_APPLE_TWIDDLED (2)
#endif

static const struct debug_named_value agx_debug_options[] = {
   {"trace",     AGX_DBG_TRACE,    "Trace the command stream"},
   {"deqp",      AGX_DBG_DEQP,     "Hacks for dEQP"},
   {"no16",      AGX_DBG_NO16,     "Disable 16-bit support"},
   {"perf",      AGX_DBG_PERF,     "Print performance warnings"},
#ifndef NDEBUG
   {"dirty",     AGX_DBG_DIRTY,    "Disable dirty tracking"},
#endif
   {"precompile",AGX_DBG_PRECOMPILE,"Precompile shaders for shader-db"},
   DEBUG_NAMED_VALUE_END
};

uint64_t agx_best_modifiers[] = {
   DRM_FORMAT_MOD_APPLE_TWIDDLED,
   DRM_FORMAT_MOD_LINEAR,
};

void agx_init_state_functions(struct pipe_context *ctx);

static struct pipe_query *
agx_create_query(struct pipe_context *ctx, unsigned query_type, unsigned index)
{
   struct agx_query *query = CALLOC_STRUCT(agx_query);

   return (struct pipe_query *)query;
}

static void
agx_destroy_query(struct pipe_context *ctx, struct pipe_query *query)
{
   FREE(query);
}

static bool
agx_begin_query(struct pipe_context *ctx, struct pipe_query *query)
{
   return true;
}

static bool
agx_end_query(struct pipe_context *ctx, struct pipe_query *query)
{
   return true;
}

static bool
agx_get_query_result(struct pipe_context *ctx,
                     struct pipe_query *query,
                     bool wait,
                     union pipe_query_result *vresult)
{
   uint64_t *result = (uint64_t*)vresult;

   *result = 0;
   return true;
}

static void
agx_set_active_query_state(struct pipe_context *pipe, bool enable)
{
}


/*
 * resource
 */

static void
agx_resource_setup(struct agx_device *dev,
                   struct agx_resource *nresource)
{
   struct pipe_resource *templ = &nresource->base;

   nresource->layout = (struct ail_layout) {
      .tiling = (nresource->modifier == DRM_FORMAT_MOD_LINEAR) ?
                AIL_TILING_LINEAR : AIL_TILING_TWIDDLED,
      .format = templ->format,
      .width_px = templ->width0,
      .height_px = templ->height0,
      .depth_px = templ->depth0 * templ->array_size,
      .sample_count_sa = MAX2(templ->nr_samples, 1),
      .levels = templ->last_level + 1,
   };
}

static struct pipe_resource *
agx_resource_from_handle(struct pipe_screen *pscreen,
                         const struct pipe_resource *templat,
                         struct winsys_handle *whandle,
                         unsigned usage)
{
   struct agx_device *dev = agx_device(pscreen);
   struct agx_resource *rsc;
   struct pipe_resource *prsc;

   assert(whandle->type == WINSYS_HANDLE_TYPE_FD);

   rsc = CALLOC_STRUCT(agx_resource);
   if (!rsc)
      return NULL;

   rsc->modifier = whandle->modifier == DRM_FORMAT_MOD_INVALID ?
                   DRM_FORMAT_MOD_LINEAR : whandle->modifier;

   /* We need strides to be aligned. ail asserts this, but we want to fail
    * gracefully so the app can handle the error.
    */
   if (rsc->modifier == DRM_FORMAT_MOD_LINEAR && (whandle->stride % 16) != 0) {
      FREE(rsc);
      return false;
   }

   prsc = &rsc->base;

   *prsc = *templat;

   pipe_reference_init(&prsc->reference, 1);
   prsc->screen = pscreen;

   rsc->bo = agx_bo_import(dev, whandle->handle);
   /* Sometimes an import can fail e.g. on an invalid buffer fd, out of
   * memory space to mmap it etc.
   */
   if (!rsc->bo) {
            FREE(rsc);
            return NULL;
   }

   agx_resource_setup(dev, rsc);

   if (rsc->layout.tiling == AIL_TILING_LINEAR)
      rsc->layout.linear_stride_B = whandle->stride;
   else if (whandle->stride != ail_get_wsi_stride_B(&rsc->layout, 0))
      return NULL;

   assert(whandle->offset == 0);

   ail_make_miptree(&rsc->layout);

   if (dev->ro) {
            rsc->scanout =
                  renderonly_create_gpu_import_for_resource(prsc, dev->ro, NULL);
            /* failure is expected in some cases.. */
   }

   return prsc;
}

static bool
agx_resource_get_handle(struct pipe_screen *pscreen,
                        struct pipe_context *ctx,
                        struct pipe_resource *pt,
                        struct winsys_handle *handle,
                        unsigned usage)
{
   struct agx_device *dev = agx_device(pscreen);
   struct renderonly_scanout *scanout;
   struct pipe_resource *cur = pt;

   /* Even though asahi doesn't support multi-planar formats, we
    * can get here through GBM, which does. Walk the list of planes
    * to find the right one.
    */
   for (int i = 0; i < handle->plane; i++) {
      cur = cur->next;
      if (!cur)
         return false;
   }

   struct agx_resource *rsrc = (struct agx_resource *)cur;
   scanout = rsrc->scanout;

   if (handle->type == WINSYS_HANDLE_TYPE_KMS && dev->ro) {
      return renderonly_get_handle(scanout, handle);
   } else if (handle->type == WINSYS_HANDLE_TYPE_KMS) {
      handle->handle = rsrc->bo->handle;
   } else if (handle->type == WINSYS_HANDLE_TYPE_FD) {
      int fd = agx_bo_export(rsrc->bo);

      if (fd < 0)
         return false;

      handle->handle = fd;
   } else {
      /* Other handle types not supported */
      return false;
   }

   handle->stride = ail_get_wsi_stride_B(&rsrc->layout, 0);
   handle->size = rsrc->layout.size_B;
   handle->offset = rsrc->layout.level_offsets_B[0];
   handle->format = rsrc->layout.format;
   handle->modifier = rsrc->modifier;

   return true;
}


static bool
agx_resource_get_param(struct pipe_screen *pscreen,
                       struct pipe_context *pctx, struct pipe_resource *prsc,
                       unsigned plane, unsigned layer, unsigned level,
                       enum pipe_resource_param param,
                       unsigned usage, uint64_t *value)
{
   struct agx_resource *rsrc = (struct agx_resource *)prsc;
   struct pipe_resource *cur;
   unsigned count;

   switch (param) {
   case PIPE_RESOURCE_PARAM_STRIDE:
      *value = ail_get_wsi_stride_B(&rsrc->layout, level);
      return true;
   case PIPE_RESOURCE_PARAM_OFFSET:
      *value = rsrc->layout.level_offsets_B[level];
      return true;
   case PIPE_RESOURCE_PARAM_MODIFIER:
      *value = rsrc->modifier;
      return true;
   case PIPE_RESOURCE_PARAM_NPLANES:
      /* We don't support multi-planar formats, but we should still handle
       * this case for GBM shared resources.
       */
      for (count = 0, cur = prsc; cur; cur = cur->next)
         count++;
      *value = count;
      return true;
   default:
      return false;
   }
}

static bool
agx_is_2d(enum pipe_texture_target target)
{
   return (target == PIPE_TEXTURE_2D || target == PIPE_TEXTURE_RECT);
}

static bool
agx_linear_allowed(const struct agx_resource *pres)
{
   /* Mipmapping not allowed with linear */
   if (pres->base.last_level != 0)
      return false;

   switch (pres->base.target) {
   /* 1D is always linear */
   case PIPE_BUFFER:
   case PIPE_TEXTURE_1D:

   /* Linear textures require specifying their strides explicitly, which only
    * works for 2D textures. Rectangle textures are a special case of 2D.
    */
   case PIPE_TEXTURE_2D:
   case PIPE_TEXTURE_RECT:
      break;

   /* No other texture type can specify a stride */
   default:
      return false;
   }

   return true;
}

static bool
agx_twiddled_allowed(const struct agx_resource *pres)
{
   /* Certain binds force linear */
   if (pres->base.bind & (PIPE_BIND_DISPLAY_TARGET | PIPE_BIND_SCANOUT | PIPE_BIND_LINEAR))
      return false;

   /* Buffers must be linear, and it does not make sense to twiddle 1D */
   if (pres->base.target == PIPE_BUFFER || pres->base.target == PIPE_TEXTURE_1D)
      return false;

   /* Anything else may be twiddled */
   return true;
}

static uint64_t
agx_select_modifier_from_list(const struct agx_resource *pres,
                              const uint64_t *modifiers, int count)
{
   if (agx_twiddled_allowed(pres) &&
       drm_find_modifier(DRM_FORMAT_MOD_APPLE_TWIDDLED, modifiers, count))
      return DRM_FORMAT_MOD_APPLE_TWIDDLED;

   if (agx_linear_allowed(pres) &&
       drm_find_modifier(DRM_FORMAT_MOD_LINEAR, modifiers, count))
      return DRM_FORMAT_MOD_LINEAR;

   /* We didn't find anything */
   return DRM_FORMAT_MOD_INVALID;
}

static uint64_t
agx_select_best_modifier(const struct agx_resource *pres)
{
   if (agx_twiddled_allowed(pres))
      return DRM_FORMAT_MOD_APPLE_TWIDDLED;

   assert(agx_linear_allowed(pres));
   return DRM_FORMAT_MOD_LINEAR;
}

static struct pipe_resource *
agx_resource_create_with_modifiers(struct pipe_screen *screen,
                                   const struct pipe_resource *templ,
                                   const uint64_t *modifiers, int count)
{
   struct agx_device *dev = agx_device(screen);
   struct agx_resource *nresource;

   nresource = CALLOC_STRUCT(agx_resource);
   if (!nresource)
      return NULL;

   nresource->base = *templ;
   nresource->base.screen = screen;

   if (modifiers) {
      nresource->modifier = agx_select_modifier_from_list(nresource, modifiers, count);

      /* There may not be a matching modifier, bail if so */
      if (nresource->modifier == DRM_FORMAT_MOD_INVALID) {
         free(nresource);
         return NULL;
      }
   } else {
      nresource->modifier = agx_select_best_modifier(nresource);

      assert(nresource->modifier != DRM_FORMAT_MOD_INVALID);
   }

   nresource->mipmapped = (templ->last_level > 0);

   assert(templ->format != PIPE_FORMAT_Z24X8_UNORM &&
          templ->format != PIPE_FORMAT_Z24_UNORM_S8_UINT &&
          "u_transfer_helper should have lowered");

   agx_resource_setup(dev, nresource);

   pipe_reference_init(&nresource->base.reference, 1);

   struct sw_winsys *winsys = ((struct agx_screen *) screen)->winsys;

   ail_make_miptree(&nresource->layout);

   if (dev->ro && (templ->bind & PIPE_BIND_SCANOUT)) {
      struct winsys_handle handle;
      assert(util_format_get_blockwidth(templ->format) == 1);
      assert(util_format_get_blockheight(templ->format) == 1);

      unsigned width = templ->width0;
      unsigned stride = templ->width0 * util_format_get_blocksize(templ->format);
      unsigned size = nresource->layout.size_B;
      unsigned effective_rows = DIV_ROUND_UP(size, stride);

      struct pipe_resource scanout_tmpl = {
            .target = nresource->base.target,
            .format = templ->format,
            .width0 = width,
            .height0 = effective_rows,
            .depth0 = 1,
            .array_size = 1,
      };

      nresource->scanout = renderonly_scanout_for_resource(&scanout_tmpl,
                                                   dev->ro,
                                                   &handle);

      if (!nresource->scanout) {
            fprintf(stderr, "Failed to create scanout resource\n");
            free(nresource);
            return NULL;
      }
      assert(handle.type == WINSYS_HANDLE_TYPE_FD);
      nresource->bo = agx_bo_import(dev, handle.handle);
      close(handle.handle);

      if (!nresource->bo) {
            free(nresource);
            return NULL;
      }

      return &nresource->base;
   }

   if (winsys && templ->bind & PIPE_BIND_DISPLAY_TARGET) {
      unsigned width = templ->width0;
      unsigned height = templ->height0;

      if (nresource->layout.tiling == AIL_TILING_TWIDDLED) {
         width = ALIGN_POT(width, 64);
         height = ALIGN_POT(height, 64);
      }

      nresource->dt = winsys->displaytarget_create(winsys,
                      templ->bind,
                      templ->format,
                      width,
                      height,
                      64,
                      NULL /*map_front_private*/,
                      &nresource->dt_stride);

      if (nresource->layout.tiling == AIL_TILING_LINEAR)
         nresource->layout.linear_stride_B = nresource->dt_stride;

      if (nresource->dt == NULL) {
         FREE(nresource);
         return NULL;
      }
   }

   nresource->bo = agx_bo_create(dev, nresource->layout.size_B, AGX_MEMORY_TYPE_FRAMEBUFFER);

   if (!nresource->bo) {
      FREE(nresource);
      return NULL;
   }

   return &nresource->base;
}

static struct pipe_resource *
agx_resource_create(struct pipe_screen *screen,
                    const struct pipe_resource *templ)
{
   return agx_resource_create_with_modifiers(screen, templ, NULL, 0);
}

static void
agx_resource_destroy(struct pipe_screen *screen,
                     struct pipe_resource *prsrc)
{
   struct agx_resource *rsrc = (struct agx_resource *)prsrc;
   struct agx_screen *agx_screen = (struct agx_screen*)screen;

   if (rsrc->dt) {
      /* display target */
      struct sw_winsys *winsys = agx_screen->winsys;
      winsys->displaytarget_destroy(winsys, rsrc->dt);
   }

   if (rsrc->scanout)
      renderonly_scanout_destroy(rsrc->scanout, agx_screen->dev.ro);

   agx_bo_unreference(rsrc->bo);
   FREE(rsrc);
}


/*
 * transfer
 */

static void
agx_transfer_flush_region(struct pipe_context *pipe,
                          struct pipe_transfer *transfer,
                          const struct pipe_box *box)
{
}

/*
 * Perform the required synchronization before a transfer_map operation can
 * complete. This may require flushing batches.
 */
static void
agx_prepare_for_map(struct agx_context *ctx,
                    struct agx_resource *rsrc,
                    unsigned level,
                    unsigned usage,  /* a combination of PIPE_MAP_x */
                    const struct pipe_box *box)
{
   /* If the access is unsynchronized, there's nothing to do */
   if (usage & PIPE_MAP_UNSYNCHRONIZED)
      return;

   agx_flush_writer(ctx, rsrc, "Unsynchronized transfer");

   if (usage & PIPE_MAP_WRITE)
      agx_flush_readers(ctx, rsrc, "Unsynchronized read");
}

static void *
agx_transfer_map(struct pipe_context *pctx,
                 struct pipe_resource *resource,
                 unsigned level,
                 unsigned usage,  /* a combination of PIPE_MAP_x */
                 const struct pipe_box *box,
                 struct pipe_transfer **out_transfer)
{
   struct agx_context *ctx = agx_context(pctx);
   struct agx_resource *rsrc = agx_resource(resource);

   /* Can't map tiled/compressed directly */
   if ((usage & PIPE_MAP_DIRECTLY) && rsrc->modifier != DRM_FORMAT_MOD_LINEAR)
      return NULL;

   agx_prepare_for_map(ctx, rsrc, level, usage, box);

   struct agx_transfer *transfer = CALLOC_STRUCT(agx_transfer);
   transfer->base.level = level;
   transfer->base.usage = usage;
   transfer->base.box = *box;

   pipe_resource_reference(&transfer->base.resource, resource);
   *out_transfer = &transfer->base;

   if (rsrc->modifier == DRM_FORMAT_MOD_APPLE_TWIDDLED) {
      transfer->base.stride =
         util_format_get_stride(rsrc->layout.format, box->width);

      transfer->base.layer_stride =
         util_format_get_2d_size(rsrc->layout.format, transfer->base.stride,
                                 box->height);

      transfer->map = calloc(transfer->base.layer_stride, box->depth);

      if ((usage & PIPE_MAP_READ) && BITSET_TEST(rsrc->data_valid, level)) {
         for (unsigned z = 0; z < box->depth; ++z) {
            uint8_t *map = agx_map_texture_cpu(rsrc, level, box->z + z);
            uint8_t *dst = (uint8_t *) transfer->map +
                           transfer->base.layer_stride * z;

            ail_detile(map, dst, &rsrc->layout, level, transfer->base.stride,
                       box->x, box->y, box->width, box->height);
         }
      }

      return transfer->map;
   } else {
      assert (rsrc->modifier == DRM_FORMAT_MOD_LINEAR);

      transfer->base.stride = ail_get_linear_stride_B(&rsrc->layout, level);
      transfer->base.layer_stride = rsrc->layout.layer_stride_B;

      /* Be conservative for direct writes */
      if ((usage & PIPE_MAP_WRITE) &&
          (usage & (PIPE_MAP_DIRECTLY | PIPE_MAP_PERSISTENT | PIPE_MAP_COHERENT)))
      {
         BITSET_SET(rsrc->data_valid, level);
      }

      uint32_t offset = ail_get_linear_pixel_B(&rsrc->layout, level, box->x,
                                               box->y, box->z);

      return ((uint8_t *) rsrc->bo->ptr.cpu) + offset;
   }
}

static void
agx_transfer_unmap(struct pipe_context *pctx,
                   struct pipe_transfer *transfer)
{
   /* Gallium expects writeback here, so we tile */

   struct agx_transfer *trans = agx_transfer(transfer);
   struct pipe_resource *prsrc = transfer->resource;
   struct agx_resource *rsrc = (struct agx_resource *) prsrc;

   if (transfer->usage & PIPE_MAP_WRITE)
      BITSET_SET(rsrc->data_valid, transfer->level);

   /* Tiling will occur in software from a staging cpu buffer */
   if ((transfer->usage & PIPE_MAP_WRITE) &&
         rsrc->modifier == DRM_FORMAT_MOD_APPLE_TWIDDLED) {
      assert(trans->map != NULL);

      for (unsigned z = 0; z < transfer->box.depth; ++z) {
         uint8_t *map = agx_map_texture_cpu(rsrc, transfer->level,
               transfer->box.z + z);
         uint8_t *src = (uint8_t *) trans->map +
                        transfer->layer_stride * z;

         ail_tile(map, src, &rsrc->layout, transfer->level,
                  transfer->stride, transfer->box.x, transfer->box.y,
                  transfer->box.width, transfer->box.height);
      }
   }

   /* Free the transfer */
   free(trans->map);
   pipe_resource_reference(&transfer->resource, NULL);
   FREE(transfer);
}

/*
 * clear/copy
 */
static void
agx_clear(struct pipe_context *pctx, unsigned buffers, const struct pipe_scissor_state *scissor_state,
          const union pipe_color_union *color, double depth, unsigned stencil)
{
   struct agx_context *ctx = agx_context(pctx);
   struct agx_batch *batch = agx_get_batch(ctx);

   unsigned fastclear = buffers & ~(batch->draw | batch->load);
   unsigned slowclear = buffers & ~fastclear;

   assert(scissor_state == NULL && "we don't support PIPE_CAP_CLEAR_SCISSORED");

   /* Fast clears configure the batch */
   for (unsigned rt = 0; rt < PIPE_MAX_COLOR_BUFS; ++rt) {
      if (!(fastclear & (PIPE_CLEAR_COLOR0 << rt)))
         continue;

      static_assert(sizeof(color->f) == 16, "mismatched structure");

      batch->uploaded_clear_color[rt] =
         agx_pool_upload_aligned(&batch->pool, color->f, sizeof(color->f), 16);
   }

   if (fastclear & PIPE_CLEAR_DEPTH)
      batch->clear_depth = depth;

   if (fastclear & PIPE_CLEAR_STENCIL)
      batch->clear_stencil = stencil;

   /* Slow clears draw a fullscreen rectangle */
   if (slowclear) {
      agx_blitter_save(ctx, ctx->blitter, false /* render cond */);
      util_blitter_clear(ctx->blitter, ctx->framebuffer.width,
                         ctx->framebuffer.height,
                         util_framebuffer_get_num_layers(&ctx->framebuffer),
                         slowclear, color, depth, stencil,
                         util_framebuffer_get_num_samples(&ctx->framebuffer) > 1);
   }

   batch->clear |= fastclear;
   assert((batch->draw & slowclear) == slowclear);
}

static void
agx_flush_resource(struct pipe_context *ctx,
                   struct pipe_resource *resource)
{
   agx_flush_writer(agx_context(ctx), agx_resource(resource), "flush_resource");
}

/*
 * context
 */
static void
agx_flush(struct pipe_context *pctx,
          struct pipe_fence_handle **fence,
          unsigned flags)
{
   struct agx_context *ctx = agx_context(pctx);

   if (fence)
      *fence = NULL;

   agx_flush_all(ctx, "Gallium flush");
}

void
agx_flush_batch(struct agx_context *ctx, struct agx_batch *batch)
{
   struct agx_device *dev = agx_device(ctx->base.screen);

   assert(agx_batch_is_active(batch));

   /* Nothing to do */
   if (!(batch->draw | batch->clear)) {
      agx_batch_cleanup(ctx, batch);
      return;
   }

   /* Finalize the encoder */
   uint8_t stop[5 + 64] = { 0x00, 0x00, 0x00, 0xc0, 0x00 };
   memcpy(batch->encoder_current, stop, sizeof(stop));

   uint64_t pipeline_background = agx_build_meta(batch, false, false);
   uint64_t pipeline_background_partial = agx_build_meta(batch, false, true);
   uint64_t pipeline_store = agx_build_meta(batch, true, false);

   bool clear_pipeline_textures = false;

   for (unsigned i = 0; i < batch->key.nr_cbufs; ++i) {
      struct pipe_surface *surf = batch->key.cbufs[i];

      if (surf && surf->texture) {
         struct agx_resource *rt = agx_resource(surf->texture);
         BITSET_SET(rt->data_valid, surf->u.tex.level);

         if (!(batch->clear & (PIPE_CLEAR_COLOR0 << i)))
            clear_pipeline_textures = true;
      }
   }

   struct agx_resource *zbuf = batch->key.zsbuf ?
      agx_resource(batch->key.zsbuf->texture) : NULL;

   if (zbuf) {
      unsigned level = batch->key.zsbuf->u.tex.level;
      BITSET_SET(zbuf->data_valid, level);

      if (zbuf->separate_stencil)
         BITSET_SET(zbuf->separate_stencil->data_valid, level);
   }

   /* BO list for a given batch consists of:
    *  - BOs for the batch's pools
    *  - BOs for the encoder
    *  - BO for internal shaders
    *  - BOs added to the batch explicitly
    */
   agx_batch_add_bo(batch, batch->encoder);
   agx_batch_add_bo(batch, batch->scissor.bo);
   agx_batch_add_bo(batch, batch->depth_bias.bo);

   unsigned handle_count =
      agx_batch_num_bo(batch) +
      agx_pool_num_bos(&batch->pool) +
      agx_pool_num_bos(&batch->pipeline_pool);

   uint32_t *handles = calloc(sizeof(uint32_t), handle_count);
   unsigned handle = 0, handle_i = 0;

   AGX_BATCH_FOREACH_BO_HANDLE(batch, handle) {
      handles[handle_i++] = handle;
   }

   agx_pool_get_bo_handles(&batch->pool, handles + handle_i);
   handle_i += agx_pool_num_bos(&batch->pool);

   agx_pool_get_bo_handles(&batch->pipeline_pool, handles + handle_i);
   handle_i += agx_pool_num_bos(&batch->pipeline_pool);

   /* Size calculation should've been exact */
   assert(handle_i == handle_count);

   unsigned cmdbuf_id = agx_get_global_id(dev);
   unsigned encoder_id = agx_get_global_id(dev);

   unsigned cmdbuf_size = demo_cmdbuf(dev->cmdbuf.ptr.cpu,
               dev->cmdbuf.size,
               &batch->pool,
               &batch->key,
               batch->encoder->ptr.gpu,
               encoder_id,
               batch->scissor.bo->ptr.gpu,
               batch->depth_bias.bo->ptr.gpu,
               pipeline_background,
               pipeline_background_partial,
               pipeline_store,
               clear_pipeline_textures,
               batch->clear,
               batch->clear_depth,
               batch->clear_stencil);

   /* Generate the mapping table from the BO list */
   demo_mem_map(dev->memmap.ptr.cpu, dev->memmap.size, handles, handle_count,
                cmdbuf_id, encoder_id, cmdbuf_size);

   free(handles);

   agx_wait_queue(dev->queue);

   if (dev->debug & AGX_DBG_TRACE) {
      agxdecode_cmdstream(dev->cmdbuf.handle, dev->memmap.handle, true);
      agxdecode_next_frame();
   }

   agx_batch_cleanup(ctx, batch);
}

static void
agx_destroy_context(struct pipe_context *pctx)
{
   struct agx_context *ctx = agx_context(pctx);

   if (pctx->stream_uploader)
      u_upload_destroy(pctx->stream_uploader);

   if (ctx->blitter)
      util_blitter_destroy(ctx->blitter);

   util_unreference_framebuffer_state(&ctx->framebuffer);

   ralloc_free(ctx);
}

static void
agx_invalidate_resource(struct pipe_context *ctx,
                        struct pipe_resource *resource)
{
}

static struct pipe_context *
agx_create_context(struct pipe_screen *screen,
                   void *priv, unsigned flags)
{
   struct agx_context *ctx = rzalloc(NULL, struct agx_context);
   struct pipe_context *pctx = &ctx->base;

   if (!ctx)
      return NULL;

   pctx->screen = screen;
   pctx->priv = priv;

   ctx->writer = _mesa_pointer_hash_table_create(ctx);

   /* Upload fixed shaders (TODO: compile them?) */

   pctx->stream_uploader = u_upload_create_default(pctx);
   if (!pctx->stream_uploader) {
      FREE(pctx);
      return NULL;
   }
   pctx->const_uploader = pctx->stream_uploader;

   pctx->destroy = agx_destroy_context;
   pctx->flush = agx_flush;
   pctx->clear = agx_clear;
   pctx->resource_copy_region = util_resource_copy_region;
   pctx->blit = agx_blit;
   pctx->flush_resource = agx_flush_resource;
   pctx->create_query = agx_create_query;
   pctx->destroy_query = agx_destroy_query;
   pctx->begin_query = agx_begin_query;
   pctx->end_query = agx_end_query;
   pctx->get_query_result = agx_get_query_result;
   pctx->set_active_query_state = agx_set_active_query_state;

   pctx->buffer_map = u_transfer_helper_transfer_map;
   pctx->buffer_unmap = u_transfer_helper_transfer_unmap;
   pctx->texture_map = u_transfer_helper_transfer_map;
   pctx->texture_unmap = u_transfer_helper_transfer_unmap;
   pctx->transfer_flush_region = u_transfer_helper_transfer_flush_region;

   pctx->buffer_subdata = u_default_buffer_subdata;
   pctx->texture_subdata = u_default_texture_subdata;
   pctx->set_debug_callback = u_default_set_debug_callback;
   pctx->invalidate_resource = agx_invalidate_resource;
   agx_init_state_functions(pctx);

   agx_meta_init(&ctx->meta, agx_device(screen), ctx);

   ctx->blitter = util_blitter_create(pctx);

   return pctx;
}

static void
agx_flush_frontbuffer(struct pipe_screen *_screen,
                      struct pipe_context *pctx,
                      struct pipe_resource *prsrc,
                      unsigned level, unsigned layer,
                      void *context_private, struct pipe_box *box)
{
   struct agx_resource *rsrc = (struct agx_resource *) prsrc;
   struct agx_screen *agx_screen = (struct agx_screen*)_screen;
   struct sw_winsys *winsys = agx_screen->winsys;

   /* Dump the framebuffer */
   assert (rsrc->dt);
   void *map = winsys->displaytarget_map(winsys, rsrc->dt, PIPE_USAGE_DEFAULT);
   assert(map != NULL);

   if (rsrc->modifier == DRM_FORMAT_MOD_APPLE_TWIDDLED) {
      ail_detile(rsrc->bo->ptr.cpu, map, &rsrc->layout, 0, rsrc->dt_stride,
                 0, 0, rsrc->base.width0, rsrc->base.height0);
   } else {
      memcpy(map, rsrc->bo->ptr.cpu, rsrc->dt_stride * rsrc->base.height0);
   }

   winsys->displaytarget_display(winsys, rsrc->dt, context_private, box);
}

static const char *
agx_get_vendor(struct pipe_screen* pscreen)
{
   return "Asahi";
}

static const char *
agx_get_device_vendor(struct pipe_screen* pscreen)
{
   return "Apple";
}

static const char *
agx_get_name(struct pipe_screen* pscreen)
{
   return "Apple M1 (G13G B0)";
}

static int
agx_get_param(struct pipe_screen* pscreen, enum pipe_cap param)
{
   bool is_deqp = agx_device(pscreen)->debug & AGX_DBG_DEQP;

   switch (param) {
   case PIPE_CAP_NPOT_TEXTURES:
   case PIPE_CAP_MIXED_COLOR_DEPTH_BITS:
   case PIPE_CAP_FRAGMENT_SHADER_TEXTURE_LOD:
   case PIPE_CAP_VERTEX_COLOR_UNCLAMPED:
   case PIPE_CAP_DEPTH_CLIP_DISABLE:
   case PIPE_CAP_MIXED_FRAMEBUFFER_SIZES:
   case PIPE_CAP_FRAGMENT_SHADER_DERIVATIVES:
   case PIPE_CAP_FRAMEBUFFER_NO_ATTACHMENT:
      return 1;

   /* We could support ARB_clip_control by toggling the clip control bit for
    * the render pass. Because this bit is for the whole render pass,
    * switching clip modes necessarily incurs a flush. This should be ok, from
    * the ARB_clip_control spec:
    *
    *         Some implementations may introduce a flush when changing the
    *         clip control state.  Hence frequent clip control changes are
    *         not recommended.
    *
    * However, this would require tuning to ensure we don't flush unnecessary
    * when using u_blitter clears, for example. As we don't yet have a use case,
    * don't expose the feature.
    */
   case PIPE_CAP_CLIP_HALFZ:
      return 0;

   case PIPE_CAP_MAX_RENDER_TARGETS:
      return 1;

   case PIPE_CAP_MAX_DUAL_SOURCE_RENDER_TARGETS:
      return 0;

   case PIPE_CAP_OCCLUSION_QUERY:
   case PIPE_CAP_PRIMITIVE_RESTART:
   case PIPE_CAP_PRIMITIVE_RESTART_FIXED_INDEX:
      return true;

   case PIPE_CAP_SAMPLER_VIEW_TARGET:
   case PIPE_CAP_TEXTURE_SWIZZLE:
   case PIPE_CAP_BLEND_EQUATION_SEPARATE:
   case PIPE_CAP_INDEP_BLEND_ENABLE:
   case PIPE_CAP_INDEP_BLEND_FUNC:
   case PIPE_CAP_ACCELERATED:
   case PIPE_CAP_UMA:
   case PIPE_CAP_TEXTURE_FLOAT_LINEAR:
   case PIPE_CAP_TEXTURE_HALF_FLOAT_LINEAR:
   case PIPE_CAP_SHADER_ARRAY_COMPONENTS:
   case PIPE_CAP_PACKED_UNIFORMS:
      return 1;

   case PIPE_CAP_VS_INSTANCEID:
   case PIPE_CAP_VERTEX_ELEMENT_INSTANCE_DIVISOR:
   case PIPE_CAP_TEXTURE_MULTISAMPLE:
   case PIPE_CAP_SURFACE_SAMPLE_COUNT:
   case PIPE_CAP_SAMPLE_SHADING:
      return is_deqp;

   case PIPE_CAP_COPY_BETWEEN_COMPRESSED_AND_PLAIN_FORMATS:
      return 0;

   case PIPE_CAP_MAX_STREAM_OUTPUT_BUFFERS:
      return is_deqp ? PIPE_MAX_SO_BUFFERS : 0;

   case PIPE_CAP_MAX_STREAM_OUTPUT_SEPARATE_COMPONENTS:
   case PIPE_CAP_MAX_STREAM_OUTPUT_INTERLEAVED_COMPONENTS:
      return is_deqp ? PIPE_MAX_SO_OUTPUTS : 0;

   case PIPE_CAP_STREAM_OUTPUT_PAUSE_RESUME:
   case PIPE_CAP_STREAM_OUTPUT_INTERLEAVE_BUFFERS:
      return is_deqp ? 1 : 0;
 
   case PIPE_CAP_MAX_TEXTURE_ARRAY_LAYERS:
      return 256;

   case PIPE_CAP_GLSL_FEATURE_LEVEL:
   case PIPE_CAP_GLSL_FEATURE_LEVEL_COMPATIBILITY:
      return is_deqp ? 330 : 130;
   case PIPE_CAP_ESSL_FEATURE_LEVEL:
      return is_deqp ? 320 : 120;

   case PIPE_CAP_CONSTANT_BUFFER_OFFSET_ALIGNMENT:
      return 16;

   case PIPE_CAP_MAX_TEXEL_BUFFER_ELEMENTS_UINT:
      return 65536;

   case PIPE_CAP_TEXTURE_BUFFER_OFFSET_ALIGNMENT:
      return 64;

   case PIPE_CAP_VERTEX_BUFFER_STRIDE_4BYTE_ALIGNED_ONLY:
      return 1;

   case PIPE_CAP_MAX_TEXTURE_2D_SIZE:
      return 16384;
   case PIPE_CAP_MAX_TEXTURE_3D_LEVELS:
   case PIPE_CAP_MAX_TEXTURE_CUBE_LEVELS:
      return 13;

   case PIPE_CAP_FS_COORD_ORIGIN_LOWER_LEFT:
      return 0;

   case PIPE_CAP_FS_COORD_ORIGIN_UPPER_LEFT:
   case PIPE_CAP_FS_COORD_PIXEL_CENTER_HALF_INTEGER:
   case PIPE_CAP_FS_COORD_PIXEL_CENTER_INTEGER:
   case PIPE_CAP_TGSI_TEXCOORD:
   case PIPE_CAP_FS_FACE_IS_INTEGER_SYSVAL:
   case PIPE_CAP_FS_POSITION_IS_SYSVAL:
   case PIPE_CAP_SEAMLESS_CUBE_MAP:
   case PIPE_CAP_SEAMLESS_CUBE_MAP_PER_TEXTURE:
      return true;
   case PIPE_CAP_FS_POINT_IS_SYSVAL:
      return false;

   case PIPE_CAP_MAX_VERTEX_ELEMENT_SRC_OFFSET:
      return 0xffff;

   case PIPE_CAP_TEXTURE_TRANSFER_MODES:
      return 0;

   case PIPE_CAP_ENDIANNESS:
      return PIPE_ENDIAN_LITTLE;

   case PIPE_CAP_VIDEO_MEMORY: {
      uint64_t system_memory;

      if (!os_get_total_physical_memory(&system_memory))
         return 0;

      return (int)(system_memory >> 20);
   }

   case PIPE_CAP_SHADER_BUFFER_OFFSET_ALIGNMENT:
      return 4;

   case PIPE_CAP_MAX_VARYINGS:
      return 16;

   case PIPE_CAP_FLATSHADE:
   case PIPE_CAP_TWO_SIDED_COLOR:
   case PIPE_CAP_ALPHA_TEST:
   case PIPE_CAP_POINT_SIZE_FIXED:
   case PIPE_CAP_CLIP_PLANES:
   case PIPE_CAP_NIR_IMAGES_AS_DEREF:
      return 0;

   case PIPE_CAP_SHAREABLE_SHADERS:
      return 1;

   default:
      return u_pipe_screen_get_param_defaults(pscreen, param);
   }
}

static float
agx_get_paramf(struct pipe_screen* pscreen,
               enum pipe_capf param)
{
   switch (param) {
   case PIPE_CAPF_MIN_LINE_WIDTH:
   case PIPE_CAPF_MIN_LINE_WIDTH_AA:
   case PIPE_CAPF_MIN_POINT_SIZE:
   case PIPE_CAPF_MIN_POINT_SIZE_AA:
      return 1;

   case PIPE_CAPF_POINT_SIZE_GRANULARITY:
   case PIPE_CAPF_LINE_WIDTH_GRANULARITY:
      return 0.1;

   case PIPE_CAPF_MAX_LINE_WIDTH:
   case PIPE_CAPF_MAX_LINE_WIDTH_AA:
      return 16.0; /* Off-by-one fixed point 4:4 encoding */

   case PIPE_CAPF_MAX_POINT_SIZE:
   case PIPE_CAPF_MAX_POINT_SIZE_AA:
      return 511.95f;

   case PIPE_CAPF_MAX_TEXTURE_ANISOTROPY:
      return 16.0;

   case PIPE_CAPF_MAX_TEXTURE_LOD_BIAS:
      return 16.0; /* arbitrary */

   case PIPE_CAPF_MIN_CONSERVATIVE_RASTER_DILATE:
   case PIPE_CAPF_MAX_CONSERVATIVE_RASTER_DILATE:
   case PIPE_CAPF_CONSERVATIVE_RASTER_DILATE_GRANULARITY:
      return 0.0f;

   default:
      debug_printf("Unexpected PIPE_CAPF %d query\n", param);
      return 0.0;
   }
}

static int
agx_get_shader_param(struct pipe_screen* pscreen,
                     enum pipe_shader_type shader,
                     enum pipe_shader_cap param)
{
   bool is_no16 = agx_device(pscreen)->debug & AGX_DBG_NO16;

   if (shader != PIPE_SHADER_VERTEX &&
       shader != PIPE_SHADER_FRAGMENT)
      return 0;

   /* this is probably not totally correct.. but it's a start: */
   switch (param) {
   case PIPE_SHADER_CAP_MAX_INSTRUCTIONS:
   case PIPE_SHADER_CAP_MAX_ALU_INSTRUCTIONS:
   case PIPE_SHADER_CAP_MAX_TEX_INSTRUCTIONS:
   case PIPE_SHADER_CAP_MAX_TEX_INDIRECTIONS:
      return 16384;

   case PIPE_SHADER_CAP_MAX_CONTROL_FLOW_DEPTH:
      return 1024;

   case PIPE_SHADER_CAP_MAX_INPUTS:
      return 16;

   case PIPE_SHADER_CAP_MAX_OUTPUTS:
      return shader == PIPE_SHADER_FRAGMENT ? 4 : 16;

   case PIPE_SHADER_CAP_MAX_TEMPS:
      return 256; /* GL_MAX_PROGRAM_TEMPORARIES_ARB */

   case PIPE_SHADER_CAP_MAX_CONST_BUFFER0_SIZE:
      return 16 * 1024 * sizeof(float);

   case PIPE_SHADER_CAP_MAX_CONST_BUFFERS:
      return 16;

   case PIPE_SHADER_CAP_CONT_SUPPORTED:
      return 0;

   case PIPE_SHADER_CAP_INDIRECT_INPUT_ADDR:
   case PIPE_SHADER_CAP_INDIRECT_OUTPUT_ADDR:
   case PIPE_SHADER_CAP_INDIRECT_TEMP_ADDR:
   case PIPE_SHADER_CAP_SUBROUTINES:
   case PIPE_SHADER_CAP_TGSI_SQRT_SUPPORTED:
      return 0;

   case PIPE_SHADER_CAP_INDIRECT_CONST_ADDR:
   case PIPE_SHADER_CAP_INTEGERS:
      return true;

   case PIPE_SHADER_CAP_FP16:
   case PIPE_SHADER_CAP_GLSL_16BIT_CONSTS:
   case PIPE_SHADER_CAP_FP16_DERIVATIVES:
   case PIPE_SHADER_CAP_FP16_CONST_BUFFERS:
      return !is_no16;
   case PIPE_SHADER_CAP_INT16:
      /* GLSL compiler is broken. Flip this on when Panfrost does. */
      return false;

   case PIPE_SHADER_CAP_INT64_ATOMICS:
   case PIPE_SHADER_CAP_DROUND_SUPPORTED:
   case PIPE_SHADER_CAP_DFRACEXP_DLDEXP_SUPPORTED:
   case PIPE_SHADER_CAP_LDEXP_SUPPORTED:
   case PIPE_SHADER_CAP_TGSI_ANY_INOUT_DECL_RANGE:
      return 0;

   case PIPE_SHADER_CAP_MAX_TEXTURE_SAMPLERS:
   case PIPE_SHADER_CAP_MAX_SAMPLER_VIEWS:
      return 16; /* XXX: How many? */

   case PIPE_SHADER_CAP_PREFERRED_IR:
      return PIPE_SHADER_IR_NIR;

   case PIPE_SHADER_CAP_SUPPORTED_IRS:
      return (1 << PIPE_SHADER_IR_NIR) | (1 << PIPE_SHADER_IR_NIR_SERIALIZED);

   case PIPE_SHADER_CAP_MAX_SHADER_BUFFERS:
   case PIPE_SHADER_CAP_MAX_SHADER_IMAGES:
   case PIPE_SHADER_CAP_MAX_HW_ATOMIC_COUNTERS:
   case PIPE_SHADER_CAP_MAX_HW_ATOMIC_COUNTER_BUFFERS:
      return 0;

   default:
      /* Other params are unknown */
      return 0;
   }

   return 0;
}

static int
agx_get_compute_param(struct pipe_screen *pscreen,
                      enum pipe_shader_ir ir_type,
                      enum pipe_compute_cap param,
                      void *ret)
{
   return 0;
}

static bool
agx_is_format_supported(struct pipe_screen* pscreen,
                        enum pipe_format format,
                        enum pipe_texture_target target,
                        unsigned sample_count,
                        unsigned storage_sample_count,
                        unsigned usage)
{
   assert(target == PIPE_BUFFER ||
          target == PIPE_TEXTURE_1D ||
          target == PIPE_TEXTURE_1D_ARRAY ||
          target == PIPE_TEXTURE_2D ||
          target == PIPE_TEXTURE_2D_ARRAY ||
          target == PIPE_TEXTURE_RECT ||
          target == PIPE_TEXTURE_3D ||
          target == PIPE_TEXTURE_CUBE ||
          target == PIPE_TEXTURE_CUBE_ARRAY);

   if (sample_count > 1)
      return false;

   if (MAX2(sample_count, 1) != MAX2(storage_sample_count, 1))
      return false;

   if (usage & (PIPE_BIND_RENDER_TARGET | PIPE_BIND_SAMPLER_VIEW)) {
      enum pipe_format tex_format = format;

      /* Mimic the fixup done in create_sampler_view and u_transfer_helper so we
       * advertise GL_OES_texture_stencil8. Alternatively, we could make mesa/st
       * less stupid?
       */
      if (tex_format == PIPE_FORMAT_X24S8_UINT)
         tex_format = PIPE_FORMAT_S8_UINT;

      struct agx_pixel_format_entry ent = agx_pixel_format[tex_format];

      if (!agx_is_valid_pixel_format(tex_format))
         return false;

      if ((usage & PIPE_BIND_RENDER_TARGET) && !ent.renderable)
         return false;
   }

   /* TODO: formats */
   if (usage & PIPE_BIND_VERTEX_BUFFER) {
      switch (format) {
      case PIPE_FORMAT_R32_FLOAT:
      case PIPE_FORMAT_R32G32_FLOAT:
      case PIPE_FORMAT_R32G32B32_FLOAT:
      case PIPE_FORMAT_R32G32B32A32_FLOAT:
         break;
      default:
         return false;
      }
   }

   if (usage & PIPE_BIND_DEPTH_STENCIL) {
      switch (format) {
      /* natively supported
       * TODO: we could also support Z16_UNORM */
      case PIPE_FORMAT_Z32_FLOAT:
      case PIPE_FORMAT_S8_UINT:

      /* lowered by u_transfer_helper to one of the above */
      case PIPE_FORMAT_Z24X8_UNORM:
      case PIPE_FORMAT_Z24_UNORM_S8_UINT:
      case PIPE_FORMAT_Z32_FLOAT_S8X24_UINT:
         break;

      default:
         return false;
      }
   }

   return true;
}

static void
agx_query_dmabuf_modifiers(struct pipe_screen *screen,
                           enum pipe_format format, int max,
                           uint64_t *modifiers,
                           unsigned int *external_only, int *out_count)
{
   int i;

   if (max == 0) {
      *out_count = ARRAY_SIZE(agx_best_modifiers);
      return;
   }

   for (i = 0; i < ARRAY_SIZE(agx_best_modifiers) && i < max; i++) {
      if (external_only)
         external_only[i] = 0;

      modifiers[i] = agx_best_modifiers[i];
   }

   /* Return the number of modifiers copied */
   *out_count = i;
}

static bool
agx_is_dmabuf_modifier_supported(struct pipe_screen *screen,
                                 uint64_t modifier, enum pipe_format format,
                                 bool *external_only)
{
   if (external_only)
      *external_only = false;

   switch (modifier) {
   case DRM_FORMAT_MOD_APPLE_TWIDDLED:
   case DRM_FORMAT_MOD_LINEAR:
      return true;
   default:
      return false;
   }
}

static void
agx_destroy_screen(struct pipe_screen *screen)
{
   u_transfer_helper_destroy(screen->transfer_helper);
   agx_close_device(agx_device(screen));
   ralloc_free(screen);
}

static void
agx_fence_reference(struct pipe_screen *screen,
                    struct pipe_fence_handle **ptr,
                    struct pipe_fence_handle *fence)
{
}

static bool
agx_fence_finish(struct pipe_screen *screen,
                 struct pipe_context *ctx,
                 struct pipe_fence_handle *fence,
                 uint64_t timeout)
{
   return true;
}

static const void *
agx_get_compiler_options(struct pipe_screen *pscreen,
                         enum pipe_shader_ir ir,
                         enum pipe_shader_type shader)
{
   return &agx_nir_options;
}

static void
agx_resource_set_stencil(struct pipe_resource *prsrc,
                         struct pipe_resource *stencil)
{
   agx_resource(prsrc)->separate_stencil = agx_resource(stencil);
}

static struct pipe_resource *
agx_resource_get_stencil(struct pipe_resource *prsrc)
{
   return (struct pipe_resource *) agx_resource(prsrc)->separate_stencil;
}

static enum pipe_format
agx_resource_get_internal_format(struct pipe_resource *prsrc)
{
   return agx_resource(prsrc)->layout.format;
}

static const struct u_transfer_vtbl transfer_vtbl = {
   .resource_create          = agx_resource_create,
   .resource_destroy         = agx_resource_destroy,
   .transfer_map             = agx_transfer_map,
   .transfer_unmap           = agx_transfer_unmap,
   .transfer_flush_region    = agx_transfer_flush_region,
   .get_internal_format      = agx_resource_get_internal_format,
   .set_stencil              = agx_resource_set_stencil,
   .get_stencil              = agx_resource_get_stencil,
};

struct pipe_screen *
agx_screen_create(int fd, struct renderonly *ro, struct sw_winsys *winsys)
{
   struct agx_screen *agx_screen;
   struct pipe_screen *screen;

   agx_screen = rzalloc(NULL, struct agx_screen);
   if (!agx_screen)
      return NULL;

   screen = &agx_screen->pscreen;
   agx_screen->winsys = winsys;

   /* Set debug before opening */
   agx_screen->dev.debug =
      debug_get_flags_option("ASAHI_MESA_DEBUG", agx_debug_options, 0);

   agx_screen->dev.fd = fd;
   agx_screen->dev.ro = ro;

   /* Try to open an AGX device */
   if (!agx_open_device(screen, &agx_screen->dev)) {
      ralloc_free(agx_screen);
      return NULL;
   }

   if (agx_screen->dev.debug & AGX_DBG_DEQP) {
      /* You're on your own. */
      static bool warned_about_hacks = false;

      if (!warned_about_hacks) {
         fprintf(stderr, "\n------------------\n"
                         "Unsupported debug parameter set. Expect breakage.\n"
                         "Do not report bugs.\n"
                         "------------------\n\n");
         warned_about_hacks = true;
      }
   }

   screen->destroy = agx_destroy_screen;
   screen->get_name = agx_get_name;
   screen->get_vendor = agx_get_vendor;
   screen->get_device_vendor = agx_get_device_vendor;
   screen->get_param = agx_get_param;
   screen->get_shader_param = agx_get_shader_param;
   screen->get_compute_param = agx_get_compute_param;
   screen->get_paramf = agx_get_paramf;
   screen->is_format_supported = agx_is_format_supported;
   screen->query_dmabuf_modifiers = agx_query_dmabuf_modifiers;
   screen->is_dmabuf_modifier_supported = agx_is_dmabuf_modifier_supported;
   screen->context_create = agx_create_context;
   screen->resource_from_handle = agx_resource_from_handle;
   screen->resource_get_handle = agx_resource_get_handle;
   screen->resource_get_param = agx_resource_get_param;
   screen->resource_create_with_modifiers = agx_resource_create_with_modifiers;
   screen->flush_frontbuffer = agx_flush_frontbuffer;
   screen->get_timestamp = u_default_get_timestamp;
   screen->fence_reference = agx_fence_reference;
   screen->fence_finish = agx_fence_finish;
   screen->get_compiler_options = agx_get_compiler_options;

   screen->resource_create = u_transfer_helper_resource_create;
   screen->resource_destroy = u_transfer_helper_resource_destroy;
   screen->transfer_helper = u_transfer_helper_create(&transfer_vtbl,
                                                      U_TRANSFER_HELPER_SEPARATE_Z32S8 |
                                                      U_TRANSFER_HELPER_SEPARATE_STENCIL |
                                                      U_TRANSFER_HELPER_MSAA_MAP |
                                                      U_TRANSFER_HELPER_Z24_IN_Z32F);

   return screen;
}
