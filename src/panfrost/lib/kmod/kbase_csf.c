/*
 * Copyright © 2026 Collabora, Ltd.
 * SPDX-License-Identifier: MIT
 */

#include <errno.h>
#include <poll.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include "c11/threads.h"
#include <unistd.h>

#include "drm-uapi/mali_kbase_ioctl.h"
#include "drm-uapi/panthor_drm.h"
#include "kbase_csf.h"
#include "kbase_kmod.h"
#include "pan_kmod.h"
#include "util/macros.h"
#include "util/u_atomic.h"

#define CSF_PAGE_SIZE 4096u
#define CSF_MAX_GROUPS 256u
#define CSF_MAX_STREAMS 65536u
#define CSF_MAX_RING_SIZE (1024u * 1024u)
#define CSF_MAX_PUBLISH 64u
#define CSF_SEQNO_SIZE 4096u
#define CSF_EVENT_SIZE 64u

struct kbase_csf_context {
   atomic_uint refs;
   atomic_bool cancelled;
   atomic_bool lost;
   mtx_t lock;
   mtx_t reader_lock;
   struct pan_kmod_dev *dev;
   struct pan_kmod_vm *vm;
   void *reg_page;
   struct kbase_csf_caps caps;
};

struct kbase_csf_queue {
   atomic_uint refs;
   atomic_bool cancelled;
   struct kbase_csf_context *ctx;
   struct pan_kmod_bo *ring;
   struct pan_kmod_bo *seqno;
   void *user_io;
   void *ring_map;
   void *seqno_map;
   volatile uint64_t *insert;
   volatile uint64_t *extract;
   uint64_t ring_va;
   uint64_t seqno_va;
   uint64_t prepared_insert;
   uint64_t prepared_target;
   uint64_t published_insert;
   uint64_t published_target;
   uint8_t group_handle;
   uint8_t group_valid;
   uint8_t queue_registered;
   uint8_t quarantined;
};

static bool
ctx_is_lost(struct kbase_csf_context *ctx)
{
   return atomic_load_explicit(&ctx->lost, memory_order_acquire) ||
          atomic_load_explicit(&ctx->cancelled, memory_order_acquire);
}

static void
ctx_mark_lost(struct kbase_csf_context *ctx)
{
   atomic_store_explicit(&ctx->lost, true, memory_order_release);
}

static int
csf_ioctl(struct kbase_csf_context *ctx, unsigned long request, void *arg)
{
   return ioctl(ctx->dev->fd, request, arg);
}

static void
context_destroy(struct kbase_csf_context *ctx)
{
   if (ctx->reg_page && ctx->reg_page != MAP_FAILED)
      munmap(ctx->reg_page, CSF_PAGE_SIZE);
   if (ctx->vm)
      pan_kmod_vm_destroy(ctx->vm);
   if (ctx->dev)
      pan_kmod_dev_destroy(ctx->dev);
   mtx_destroy(&ctx->reader_lock);
   mtx_destroy(&ctx->lock);
   free(ctx);
}

struct kbase_csf_context *
kbase_csf_context_create(struct pan_kmod_dev *dev)
{
   if (!dev || !kbase_kmod_is_csf(dev)) {
      errno = EINVAL;
      return NULL;
   }

   struct kbase_csf_context *ctx = calloc(1, sizeof(*ctx));
   if (!ctx)
      return NULL;
   atomic_init(&ctx->refs, 1);
   atomic_init(&ctx->cancelled, false);
   atomic_init(&ctx->lost, false);
   if (mtx_init(&ctx->lock, mtx_plain) != thrd_success) {
      free(ctx);
      errno = ENOMEM;
      return NULL;
   }
   if (mtx_init(&ctx->reader_lock, mtx_plain) != thrd_success) {
      mtx_destroy(&ctx->lock);
      free(ctx);
      errno = ENOMEM;
      return NULL;
   }
   ctx->dev = dev;

   ctx->vm = pan_kmod_vm_create(dev, PAN_KMOD_VM_FLAG_AUTO_VA, 0, 0);
   if (!ctx->vm)
      goto fail;

   union kbase_ioctl_cs_get_glb_iface req = {0};
   if (csf_ioctl(ctx, KBASE_IOCTL_CS_GET_GLB_IFACE, &req))
      goto fail;
   uint32_t groups = req.out.group_num;
   uint32_t total_streams = req.out.total_stream_num;
   if (!groups || groups > CSF_MAX_GROUPS || !total_streams ||
       total_streams > CSF_MAX_STREAMS) {
      errno = EPROTO;
      goto fail;
   }

   struct basep_cs_group_control *group_controls =
      calloc(groups, sizeof(*group_controls));
   struct basep_cs_stream_control *stream_controls =
      calloc(total_streams, sizeof(*stream_controls));
   if (!group_controls || !stream_controls) {
      free(group_controls);
      free(stream_controls);
      goto fail;
   }

   req = (union kbase_ioctl_cs_get_glb_iface){0};
   req.in.max_group_num = groups;
   req.in.max_total_stream_num = total_streams;
   req.in.groups_ptr = (uintptr_t)group_controls;
   req.in.streams_ptr = (uintptr_t)stream_controls;
   if (csf_ioctl(ctx, KBASE_IOCTL_CS_GET_GLB_IFACE, &req)) {
      free(group_controls);
      free(stream_controls);
      goto fail;
   }

   uint32_t returned_groups = req.out.group_num;
   uint32_t returned_streams = req.out.total_stream_num;
   bool valid = returned_groups == groups && returned_streams == total_streams;
   uint64_t stream_sum = 0;
   uint32_t max_streams = 0;
   for (uint32_t i = 0; valid && i < returned_groups; i++) {
      uint32_t n = group_controls[i].stream_num;
      if (!n || n > returned_streams || stream_sum + n > returned_streams) {
         valid = false;
         break;
      }
      stream_sum += n;
      max_streams = MAX2(max_streams, n);
   }
   if (stream_sum != returned_streams)
      valid = false;
   if (!valid) {
      free(group_controls);
      free(stream_controls);
      errno = EPROTO;
      goto fail;
   }

   uint32_t stream_features = stream_controls[0].features;
   for (uint32_t i = 1; i < returned_streams; i++) {
      if (stream_controls[i].features != stream_features) {
         free(group_controls);
         free(stream_controls);
         errno = ENOTSUP;
         goto fail;
      }
   }
   ctx->caps = (struct kbase_csf_caps){
      .version = req.out.glb_version,
      .features = req.out.features,
      .group_count = returned_groups,
      .streams_per_group = max_streams,
      .stream_features = stream_features,
      .instr_features = req.out.instr_features,
   };
   free(group_controls);
   free(stream_controls);

   ctx->reg_page = mmap(NULL, CSF_PAGE_SIZE, PROT_READ, MAP_SHARED, dev->fd,
                        BASEP_MEM_CSF_USER_REG_PAGE_HANDLE);
   if (ctx->reg_page == MAP_FAILED) {
      ctx->reg_page = NULL;
      goto fail;
   }
   return ctx;

fail: {
   int error = errno ? errno : EIO;
   ctx->dev = NULL;
   context_destroy(ctx);
   errno = error;
   return NULL;
}
}

void
kbase_csf_context_ref(struct kbase_csf_context *ctx)
{
   if (ctx)
      atomic_fetch_add_explicit(&ctx->refs, 1, memory_order_relaxed);
}

void
kbase_csf_context_unref(struct kbase_csf_context *ctx)
{
   if (ctx && atomic_fetch_sub_explicit(&ctx->refs, 1, memory_order_acq_rel) == 1)
      context_destroy(ctx);
}

void
kbase_csf_context_cancel(struct kbase_csf_context *ctx)
{
   if (ctx)
      atomic_store_explicit(&ctx->cancelled, true, memory_order_release);
}

const struct kbase_csf_caps *
kbase_csf_context_caps(struct kbase_csf_context *ctx)
{
   return ctx ? &ctx->caps : NULL;
}

uint32_t
kbase_csf_context_flush_id(struct kbase_csf_context *ctx)
{
   return ctx && ctx->reg_page ?
      *(volatile uint32_t *)((uint8_t *)ctx->reg_page + CS_USER_REG_LATEST_FLUSH) : 0;
}

struct pan_kmod_dev *
kbase_csf_context_dev(struct kbase_csf_context *ctx)
{
   return ctx ? ctx->dev : NULL;
}

struct pan_kmod_vm *
kbase_csf_context_vm(struct kbase_csf_context *ctx)
{
   return ctx ? ctx->vm : NULL;
}

int
kbase_csf_context_get_csif(struct kbase_csf_context *ctx,
                           struct drm_panthor_csif_info *out)
{
   if (!ctx || !out) {
      errno = EINVAL;
      return -1;
   }

   uint32_t cs_slots = (ctx->caps.stream_features >> 8) & 0xff;

   if (!kbase_kmod_supports_panvk(ctx->dev) || cs_slots <= 3 || cs_slots > 16) {
      errno = ENOTSUP;
      return -1;
   }

   *out = (struct drm_panthor_csif_info){
      .csg_slot_count = ctx->caps.group_count,
      .cs_slot_count = ctx->caps.streams_per_group,
      .cs_reg_count = 128,
      .scoreboard_slot_count = cs_slots,
      .unpreserved_cs_reg_count = 4,
   };
   return 0;
}

static int check_notifications(struct kbase_csf_context *ctx);

int
kbase_csf_context_status(struct kbase_csf_context *ctx)
{
   if (!ctx) {
      errno = EINVAL;
      return -1;
   }
   if (atomic_load_explicit(&ctx->cancelled, memory_order_acquire)) {
      errno = ECANCELED;
      return -1;
   }
   if (atomic_load_explicit(&ctx->lost, memory_order_acquire)) {
      errno = EIO;
      return -1;
   }

   if (mtx_trylock(&ctx->reader_lock) != thrd_success)
      return 0;

   struct pollfd pfd = {.fd = ctx->dev->fd, .events = POLLIN};
   int ret = ppoll(&pfd, 1, &(struct timespec){0}, NULL);
   if (ret < 0) {
      int error = errno;
      mtx_unlock(&ctx->reader_lock);
      if (error == EINTR)
         return 0;
      ctx_mark_lost(ctx);
      errno = error;
      return -1;
   }
   if (ret > 0) {
      if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) {
         mtx_unlock(&ctx->reader_lock);
         ctx_mark_lost(ctx);
         errno = EIO;
         return -1;
      }
      if (pfd.revents & POLLIN && check_notifications(ctx)) {
         int error = errno;
         mtx_unlock(&ctx->reader_lock);
         errno = error;
         return -1;
      }
   }
   mtx_unlock(&ctx->reader_lock);
   return 0;
}

static void
queue_release_storage(struct kbase_csf_queue *queue)
{
   if (queue->seqno_map)
      pan_kmod_bo_munmap(queue->seqno, queue->seqno_map, CSF_SEQNO_SIZE);
   if (queue->ring_map)
      pan_kmod_bo_munmap(queue->ring, queue->ring_map, queue->ring->size);
   if (queue->seqno)
      pan_kmod_bo_put(queue->seqno);
   if (queue->ring)
      pan_kmod_bo_put(queue->ring);
   queue->seqno = NULL;
   queue->ring = NULL;
}

static void
queue_destroy(struct kbase_csf_queue *queue)
{
   struct kbase_csf_context *ctx = queue->ctx;
   bool safe_to_release = !queue->quarantined;
   if (!safe_to_release) {
      queue->quarantined = 1;
      return;
   }
   if (kbase_csf_queue_stop(queue)) {
      queue->quarantined = 1;
      return;
   }
   if (queue->queue_registered || queue->group_valid || queue->user_io) {
      queue->quarantined = 1;
      return;
   }
   queue_release_storage(queue);
   kbase_csf_context_unref(ctx);
   free(queue);
}

void
kbase_csf_queue_ref(struct kbase_csf_queue *queue)
{
   if (queue)
      atomic_fetch_add_explicit(&queue->refs, 1, memory_order_relaxed);
}

void
kbase_csf_queue_unref(struct kbase_csf_queue *queue)
{
   if (queue && atomic_fetch_sub_explicit(&queue->refs, 1, memory_order_acq_rel) == 1)
      queue_destroy(queue);
}

void
kbase_csf_queue_cancel(struct kbase_csf_queue *queue)
{
   if (queue)
      atomic_store_explicit(&queue->cancelled, true, memory_order_release);
}

int
kbase_csf_queue_stop(struct kbase_csf_queue *queue)
{
   if (!queue) {
      errno = EINVAL;
      return -1;
   }
   struct kbase_csf_context *ctx = queue->ctx;
   mtx_lock(&ctx->lock);
   if (!queue->queue_registered && !queue->group_valid && !queue->user_io) {
      mtx_unlock(&ctx->lock);
      return 0;
   }
   if (queue->queue_registered) {
      struct kbase_ioctl_cs_queue_terminate term = {
         .buffer_gpu_addr = queue->ring_va,
      };
      if (csf_ioctl(ctx, KBASE_IOCTL_CS_QUEUE_TERMINATE, &term)) {
         int error = errno;
         queue->quarantined = 1;
         ctx_mark_lost(ctx);
         mtx_unlock(&ctx->lock);
         errno = error;
         return -1;
      }
      queue->queue_registered = 0;
   }
   if (queue->group_valid) {
      struct kbase_ioctl_cs_queue_group_term term = {
         .group_handle = queue->group_handle,
      };
      if (csf_ioctl(ctx, KBASE_IOCTL_CS_QUEUE_GROUP_TERMINATE, &term)) {
         int error = errno;
         queue->quarantined = 1;
         ctx_mark_lost(ctx);
         mtx_unlock(&ctx->lock);
         errno = error;
         return -1;
      }
      queue->group_valid = 0;
   }
   if (queue->user_io &&
       munmap(queue->user_io, BASEP_QUEUE_NR_MMAP_USER_PAGES * CSF_PAGE_SIZE)) {
      int error = errno;
      queue->quarantined = 1;
      ctx_mark_lost(ctx);
      mtx_unlock(&ctx->lock);
      errno = error;
      return -1;
   }
   queue->user_io = NULL;
   atomic_store_explicit(&queue->cancelled, true, memory_order_release);
   mtx_unlock(&ctx->lock);
   return 0;
}

static bool
power_of_two(uint32_t n)
{
   return n && !(n & (n - 1));
}

struct kbase_csf_queue *
kbase_csf_queue_create(struct kbase_csf_context *ctx, uint32_t ring_size,
                       uint8_t priority)
{
   if (!ctx || ring_size < CSF_PAGE_SIZE || ring_size > CSF_MAX_RING_SIZE ||
       !power_of_two(ring_size) || priority > 3) {
      errno = EINVAL;
      return NULL;
   }
   if (ctx_is_lost(ctx)) {
      errno = ECANCELED;
      return NULL;
   }
   struct kbase_csf_queue *queue = calloc(1, sizeof(*queue));
   if (!queue)
      return NULL;
   atomic_init(&queue->refs, 1);
   atomic_init(&queue->cancelled, false);
   queue->ctx = ctx;
   kbase_csf_context_ref(ctx);

   queue->ring = pan_kmod_bo_alloc(ctx->dev, ctx->vm, ring_size,
                                   PAN_KMOD_BO_FLAG_WB_MMAP);
   queue->seqno = pan_kmod_bo_alloc(ctx->dev, ctx->vm, CSF_SEQNO_SIZE,
                                    PAN_KMOD_BO_FLAG_CSF_EVENT);
   if (!queue->ring || !queue->seqno)
      goto fail;
   queue->ring_map = pan_kmod_bo_mmap(queue->ring, PROT_READ | PROT_WRITE,
                                      MAP_SHARED, NULL);
   queue->seqno_map = pan_kmod_bo_mmap(queue->seqno, PROT_READ | PROT_WRITE,
                                       MAP_SHARED, NULL);
   if (queue->ring_map == MAP_FAILED || queue->seqno_map == MAP_FAILED) {
      if (queue->ring_map == MAP_FAILED)
         queue->ring_map = NULL;
      if (queue->seqno_map == MAP_FAILED)
         queue->seqno_map = NULL;
      goto fail;
   }
   queue->ring_va = kbase_kmod_bo_gpu_va(queue->ring);
   queue->seqno_va = kbase_kmod_bo_gpu_va(queue->seqno);
   if (!queue->ring_va || !queue->seqno_va || (queue->seqno_va & 15)) {
      errno = EINVAL;
      goto fail;
   }
   memset(queue->ring_map, 0, ring_size);
   memset(queue->seqno_map, 0, CSF_SEQNO_SIZE);
   if (kbase_kmod_bo_sync(queue->ring, 0, ring_size,
                          PAN_KMOD_BO_SYNC_CPU_CACHE_FLUSH) ||
       kbase_kmod_bo_sync(queue->seqno, 0, CSF_SEQNO_SIZE,
                          PAN_KMOD_BO_SYNC_CPU_CACHE_FLUSH))
      goto fail;

   struct kbase_ioctl_cs_queue_register reg = {
      .buffer_gpu_addr = queue->ring_va,
      .buffer_size = ring_size,
      .priority = priority,
   };
   if (ioctl(ctx->dev->fd, KBASE_IOCTL_CS_QUEUE_REGISTER, &reg))
      goto fail;
   queue->queue_registered = 1;

   if (pan_kmod_driver_version_at_least(&ctx->dev->driver, 1, 18)) {
      union kbase_ioctl_cs_queue_group_create_1_18 group = {
         .in = {
            .tiler_mask = 1,
            .fragment_mask = UINT64_MAX,
            .compute_mask = UINT64_MAX,
            .cs_min = 1,
            .priority = priority,
            .tiler_max = 1,
            .fragment_max = 64,
            .compute_max = 64,
         },
      };
      if (ioctl(ctx->dev->fd, KBASE_IOCTL_CS_QUEUE_GROUP_CREATE_1_18, &group))
         goto fail;
      queue->group_handle = group.out.group_handle;
   } else {
      union kbase_ioctl_cs_queue_group_create_1_6 group = {
         .in = {
            .tiler_mask = 1,
            .fragment_mask = UINT64_MAX,
            .compute_mask = UINT64_MAX,
            .cs_min = 1,
            .priority = priority,
            .tiler_max = 1,
            .fragment_max = 64,
            .compute_max = 64,
         },
      };
      if (ioctl(ctx->dev->fd, KBASE_IOCTL_CS_QUEUE_GROUP_CREATE_1_6, &group))
         goto fail;
      queue->group_handle = group.out.group_handle;
   }
   queue->group_valid = 1;

   union kbase_ioctl_cs_queue_bind bind = {0};
   bind.in.buffer_gpu_addr = queue->ring_va;
   bind.in.group_handle = queue->group_handle;
   bind.in.csi_index = 0;
   if (ioctl(ctx->dev->fd, KBASE_IOCTL_CS_QUEUE_BIND, &bind))
      goto fail;
   uint64_t cookie = bind.out.mmap_handle;
   if (!cookie || cookie > INT64_MAX || (cookie & (CSF_PAGE_SIZE - 1))) {
      errno = EINVAL;
      goto fail;
   }
   void *pages = mmap(NULL, BASEP_QUEUE_NR_MMAP_USER_PAGES * CSF_PAGE_SIZE,
                      PROT_READ | PROT_WRITE, MAP_SHARED, ctx->dev->fd, cookie);
   if (pages == MAP_FAILED)
      goto fail;
   queue->user_io = pages;
   queue->insert = (volatile uint64_t *)((uint8_t *)pages + CSF_PAGE_SIZE +
                                          CS_USER_IO_INPUT_CS_INSERT);
   queue->extract = (volatile uint64_t *)((uint8_t *)pages + 2 * CSF_PAGE_SIZE +
                                           CS_USER_IO_OUTPUT_CS_EXTRACT);
   return queue;

fail: {
   int error = errno ? errno : EIO;
   queue_destroy(queue);
   errno = error;
   return NULL;
}
}

uint64_t
kbase_csf_queue_seqno_va(struct kbase_csf_queue *queue)
{
   return queue ? queue->seqno_va : 0;
}

uint64_t
kbase_csf_queue_published_target(struct kbase_csf_queue *queue)
{
   if (!queue)
      return 0;
   mtx_lock(&queue->ctx->lock);
   uint64_t target = queue->published_target;
   mtx_unlock(&queue->ctx->lock);
   return target;
}

static int
queue_check_live(struct kbase_csf_queue *queue)
{
   if (ctx_is_lost(queue->ctx)) {
      errno = atomic_load_explicit(&queue->ctx->cancelled, memory_order_acquire) ?
                 ECANCELED : EIO;
      return -1;
   }
   if (atomic_load_explicit(&queue->cancelled, memory_order_acquire)) {
      errno = ECANCELED;
      return -1;
   }
   return 0;
}

static int
queue_prepare_locked(struct kbase_csf_queue *queue, const void *commands,
                     uint32_t size, uint64_t target)
{
   struct kbase_csf_context *ctx = queue->ctx;
   if (queue_check_live(queue))
      return -1;
   if (!commands || !size || (size & 7)) {
      errno = EINVAL;
      return -1;
   }
   if (queue->prepared_target == UINT64_MAX ||
       target != queue->prepared_target + 1) {
      errno = EINVAL;
      return -1;
   }
   uint64_t extract = *queue->extract;
   if (extract > queue->published_insert || extract > queue->prepared_insert ||
       queue->prepared_insert - extract > queue->ring->size) {
      ctx_mark_lost(ctx);
      errno = EIO;
      return -1;
   }
   uint64_t used = queue->prepared_insert - extract;
   uint64_t padded = ALIGN_POT((uint64_t)size, 64);
   uint64_t offset = queue->prepared_insert & (queue->ring->size - 1);
   uint64_t remaining = queue->ring->size - offset;
   uint64_t wrap = padded > remaining ? remaining : 0;
   if (padded > queue->ring->size) {
      errno = EINVAL;
      return -1;
   }
   if (padded + wrap > queue->ring->size - used) {
      errno = EAGAIN;
      return -1;
   }
   if (queue->prepared_insert > UINT64_MAX - padded - wrap) {
      errno = EOVERFLOW;
      return -1;
   }
   if (wrap) {
      memset((uint8_t *)queue->ring_map + offset, 0, wrap);
      queue->prepared_insert += wrap;
      offset = 0;
   }
   memcpy((uint8_t *)queue->ring_map + offset, commands, size);
   memset((uint8_t *)queue->ring_map + offset + size, 0, padded - size);
   queue->prepared_insert += padded;
   queue->prepared_target = target;
   return 0;
}

int
kbase_csf_queue_prepare(struct kbase_csf_queue *queue, const void *commands,
                        uint32_t size, uint64_t target)
{
   if (!queue || (!commands && size) || !size || (size & 7)) {
      errno = EINVAL;
      return -1;
   }
   struct kbase_csf_context *ctx = queue->ctx;
   mtx_lock(&ctx->lock);
   int ret = queue_prepare_locked(queue, commands, size, target);
   mtx_unlock(&ctx->lock);
   return ret;
}

static int
publish_locked(struct kbase_csf_context *ctx,
               struct kbase_csf_queue *const *queues, uint32_t count,
               bool *inserted)
{
   *inserted = false;
   for (uint32_t i = 0; i < count; i++) {
      struct kbase_csf_queue *q = queues[i];
      if (queue_check_live(q))
         return -1;
      if (!q->prepared_target || q->prepared_target <= q->published_target ||
          q->prepared_insert <= q->published_insert) {
         errno = EINVAL;
         return -1;
      }
   }
   for (uint32_t i = 0; i < count; i++) {
      struct kbase_csf_queue *q = queues[i];
      if (kbase_kmod_bo_sync(q->ring, 0, q->ring->size,
                             PAN_KMOD_BO_SYNC_CPU_CACHE_FLUSH))
         return -1;
   }

   atomic_thread_fence(memory_order_seq_cst);
   __sync_synchronize();
   for (uint32_t i = 0; i < count; i++) {
      *queues[i]->insert = queues[i]->prepared_insert;
      queues[i]->published_insert = queues[i]->prepared_insert;
      queues[i]->published_target = queues[i]->prepared_target;
   }
   *inserted = true;
   __sync_synchronize();
   for (uint32_t i = 0; i < count; i++) {
      struct kbase_ioctl_cs_queue_kick kick = {
         .buffer_gpu_addr = queues[i]->ring_va,
      };
      if (ioctl(ctx->dev->fd, KBASE_IOCTL_CS_QUEUE_KICK, &kick)) {
         int error = errno;
         ctx_mark_lost(ctx);
         errno = error;
         return -1;
      }
   }
   return 0;
}

static int
validate_queues(struct kbase_csf_queue *const *queues, uint32_t count,
                struct kbase_csf_context **out_ctx)
{
   if (!queues || !count || count > CSF_MAX_PUBLISH) {
      errno = EINVAL;
      return -1;
   }
   struct kbase_csf_context *ctx = queues[0] ? queues[0]->ctx : NULL;
   if (!ctx) {
      errno = EINVAL;
      return -1;
   }
   for (uint32_t i = 0; i < count; i++) {
      if (!queues[i] || queues[i]->ctx != ctx) {
         errno = EINVAL;
         return -1;
      }
      for (uint32_t j = 0; j < i; j++) {
         if (queues[j] == queues[i]) {
            errno = EINVAL;
            return -1;
         }
      }
   }
   *out_ctx = ctx;
   return 0;
}

int
kbase_csf_publish(struct kbase_csf_queue *const *queues, uint32_t count)
{
   struct kbase_csf_context *ctx;
   if (validate_queues(queues, count, &ctx))
      return -1;
   mtx_lock(&ctx->lock);
   bool inserted;
   int ret = publish_locked(ctx, queues, count, &inserted);
   mtx_unlock(&ctx->lock);
   return ret;
}

int
kbase_csf_submit(const struct kbase_csf_submit_entry *entries, uint32_t count)
{
   if (!entries || !count || count > CSF_MAX_PUBLISH) {
      errno = EINVAL;
      return -1;
   }
   struct kbase_csf_queue *queues[CSF_MAX_PUBLISH];
   uint64_t old_insert[CSF_MAX_PUBLISH];
   uint64_t old_target[CSF_MAX_PUBLISH];
   struct kbase_csf_context *ctx;
   for (uint32_t i = 0; i < count; i++)
      queues[i] = entries[i].queue;
   if (validate_queues(queues, count, &ctx))
      return -1;

   mtx_lock(&ctx->lock);
   for (uint32_t i = 0; i < count; i++) {
      old_insert[i] = queues[i]->prepared_insert;
      old_target[i] = queues[i]->prepared_target;
   }
   for (uint32_t i = 0; i < count; i++) {
      struct kbase_csf_queue *q = queues[i];
      if (queue_check_live(q))
         goto rollback;
      if (q->prepared_target == UINT64_MAX ||
          entries[i].target != q->prepared_target + 1 ||
          !entries[i].commands || !entries[i].size || (entries[i].size & 7)) {
         errno = EINVAL;
         goto rollback;
      }
   }
   for (uint32_t i = 0; i < count; i++) {
      struct kbase_csf_queue *q = queues[i];
      if (entries[i].target != q->prepared_target + 1) {
         errno = EINVAL;
         goto rollback;
      }
      if (queue_prepare_locked(q, entries[i].commands, entries[i].size,
                               entries[i].target))
         goto rollback;
   }
   {
      bool inserted;
      if (publish_locked(ctx, queues, count, &inserted)) {
         if (inserted) {
            mtx_unlock(&ctx->lock);
            errno = EIO;
            return -1;
         }
         goto rollback;
      }
   }
   mtx_unlock(&ctx->lock);
   return 0;

rollback: {
      int error = errno;
      for (uint32_t i = 0; i < count; i++) {
         queues[i]->prepared_insert = old_insert[i];
         queues[i]->prepared_target = old_target[i];
      }
      mtx_unlock(&ctx->lock);
      errno = error;
      return -1;
   }
}

static uint64_t
monotonic_ns(void)
{
   struct timespec ts;
   if (clock_gettime(CLOCK_MONOTONIC, &ts))
      return 0;
   return (uint64_t)ts.tv_sec * 1000000000ull + ts.tv_nsec;
}

static int
check_notifications(struct kbase_csf_context *ctx)
{
   struct base_csf_notification notification;
   ssize_t got = read(ctx->dev->fd, &notification, sizeof(notification));
   if (got < 0 && errno == EINTR)
      return 0;
   if (got != sizeof(notification)) {
      if (got >= 0)
         errno = EIO;
      ctx_mark_lost(ctx);
      return -1;
   }
   switch (notification.type) {
   case BASE_CSF_NOTIFICATION_EVENT:
      return 0;
   case BASE_CSF_NOTIFICATION_GPU_QUEUE_GROUP_ERROR:
      ctx_mark_lost(ctx);
      errno = EIO;
      return -1;
   case BASE_CSF_NOTIFICATION_CPU_QUEUE_DUMP:
      return 0;
   default:
      ctx_mark_lost(ctx);
      errno = EPROTO;
      return -1;
   }
}

int
kbase_csf_wait(struct kbase_csf_queue *const *queues, uint32_t count,
               const uint64_t *targets, uint64_t abs_timeout_ns)
{
   if (!queues || !targets || !count || count > CSF_MAX_PUBLISH) {
      errno = EINVAL;
      return -1;
   }
   struct kbase_csf_context *ctx = queues[0] ? queues[0]->ctx : NULL;
   if (!ctx) {
      errno = EINVAL;
      return -1;
   }
   for (uint32_t i = 0; i < count; i++) {
      if (!queues[i] || queues[i]->ctx != ctx) {
         errno = EINVAL;
         return -1;
      }
      for (uint32_t j = 0; j < i; j++) {
         if (queues[j] == queues[i]) {
            errno = EINVAL;
            return -1;
         }
      }
   }

   for (;;) {
      if (ctx_is_lost(ctx)) {
         errno = atomic_load_explicit(&ctx->cancelled, memory_order_acquire) ?
                    ECANCELED : EIO;
         return -1;
      }
      bool complete = true;
      while (mtx_trylock(&ctx->lock) != thrd_success) {
         if (ctx_is_lost(ctx)) {
            errno = ECANCELED;
            return -1;
         }
         uint64_t now = monotonic_ns();
         if (!now || now >= abs_timeout_ns)
            return 0;
         struct timespec delay = {
            .tv_sec = 0,
            .tv_nsec = MIN2(abs_timeout_ns - now, 1000000ull),
         };
         thrd_sleep(&delay, NULL);
      }
      for (uint32_t i = 0; i < count; i++) {
         struct kbase_csf_queue *q = queues[i];
         if (queue_check_live(q)) {
            mtx_unlock(&ctx->lock);
            return -1;
         }
         if (!targets[i])
            continue;
         if (targets[i] > q->published_target) {
            errno = EINVAL;
            mtx_unlock(&ctx->lock);
            return -1;
         }
         if (kbase_kmod_bo_sync(q->seqno, 0, CSF_SEQNO_SIZE,
                                PAN_KMOD_BO_SYNC_CPU_CACHE_FLUSH_AND_INVALIDATE)) {
            mtx_unlock(&ctx->lock);
            return -1;
         }
         uint64_t seqno = *(volatile uint64_t *)q->seqno_map;
         uint32_t error = *(volatile uint32_t *)((uint8_t *)q->seqno_map + 8);
         if (error) {
            ctx_mark_lost(ctx);
            errno = EIO;
            mtx_unlock(&ctx->lock);
            return -1;
         }
         if (seqno < targets[i])
            complete = false;
      }
      mtx_unlock(&ctx->lock);
      if (ctx_is_lost(ctx)) {
         errno = atomic_load_explicit(&ctx->cancelled, memory_order_acquire) ?
                    ECANCELED : EIO;
         return -1;
      }
      if (complete)
         return 1;
      uint64_t now = monotonic_ns();
      if (!now) {
         errno = EIO;
         return -1;
      }
      if (now >= abs_timeout_ns)
         return 0;

      uint64_t remaining = abs_timeout_ns - now;
      uint64_t slice = MIN2(remaining, 1000000ull);
      if (mtx_trylock(&ctx->reader_lock) != thrd_success) {
         struct timespec delay = {.tv_sec = 0, .tv_nsec = slice};
         thrd_sleep(&delay, NULL);
         continue;
      }
      if (ctx_is_lost(ctx)) {
         mtx_unlock(&ctx->reader_lock);
         errno = ECANCELED;
         return -1;
      }
      now = monotonic_ns();
      if (!now || now >= abs_timeout_ns) {
         mtx_unlock(&ctx->reader_lock);
         return 0;
      }
      slice = MIN2(abs_timeout_ns - now, 1000000ull);
      struct timespec timeout = {.tv_sec = 0, .tv_nsec = slice};
      struct pollfd pfd = {.fd = ctx->dev->fd, .events = POLLIN};
      int polled = ppoll(&pfd, 1, &timeout, NULL);
      if (polled < 0) {
         int error = errno;
         mtx_unlock(&ctx->reader_lock);
         if (error == EINTR)
            continue;
         ctx_mark_lost(ctx);
         errno = error;
         return -1;
      }
      if (polled > 0) {
         if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) {
            mtx_unlock(&ctx->reader_lock);
            ctx_mark_lost(ctx);
            errno = EIO;
            return -1;
         }
         if (pfd.revents & POLLIN) {
            if (check_notifications(ctx)) {
               int error = errno;
               mtx_unlock(&ctx->reader_lock);
               errno = error;
               return -1;
            }
         }
      }
      mtx_unlock(&ctx->reader_lock);
   }
}

/* KCPU command queue.
 *
 * The kernel processes a KCPU queue with a dedicated worker. Commands are
 * copied out of userspace at enqueue time, so the staging array only needs to
 * live for the duration of the ioctl.
 *
 * The Base interface accepts exactly one command per enqueue; ordering across
 * commands is provided by the queue itself.
 */
struct kbase_csf_kcpu {
   struct kbase_csf_context *ctx;
   base_kcpu_queue_id id;
};

struct kbase_csf_kcpu *
kbase_csf_kcpu_create(struct kbase_csf_context *ctx)
{
   if (!ctx) {
      errno = EINVAL;
      return NULL;
   }

   struct kbase_csf_kcpu *kcpu = calloc(1, sizeof(*kcpu));
   if (!kcpu)
      return NULL;

   struct kbase_ioctl_kcpu_queue_new req = {0};
   if (csf_ioctl(ctx, KBASE_IOCTL_KCPU_QUEUE_CREATE, &req)) {
      int error = errno;
      free(kcpu);
      errno = error;
      return NULL;
   }

   kcpu->ctx = ctx;
   kcpu->id = req.id;
   return kcpu;
}

void
kbase_csf_kcpu_destroy(struct kbase_csf_kcpu *kcpu)
{
   if (!kcpu)
      return;

   struct kbase_ioctl_kcpu_queue_delete req = {.id = kcpu->id};
   csf_ioctl(kcpu->ctx, KBASE_IOCTL_KCPU_QUEUE_DELETE, &req);
   free(kcpu);
}

int
kbase_csf_kcpu_enqueue(struct kbase_csf_kcpu *kcpu,
                       struct base_kcpu_command *command)
{
   if (!kcpu || !command) {
      errno = EINVAL;
      return -1;
   }

   struct kbase_ioctl_kcpu_queue_enqueue req = {
      .addr = (uint64_t)(uintptr_t)command,
      .nr_commands = 1,
      .id = kcpu->id,
   };

   return csf_ioctl(kcpu->ctx, KBASE_IOCTL_KCPU_QUEUE_ENQUEUE, &req);
}

int
kbase_csf_kcpu_fence_signal(struct kbase_csf_kcpu *kcpu, int *fd)
{
   if (!kcpu || !fd) {
      errno = EINVAL;
      return -1;
   }

   struct base_fence fence = {.basep.fd = -1, .basep.stream_fd = -1};
   struct base_kcpu_command command = {
      .type = BASE_KCPU_COMMAND_TYPE_FENCE_SIGNAL,
      .info.fence.fence = (uint64_t)(uintptr_t)&fence,
   };

   if (kbase_csf_kcpu_enqueue(kcpu, &command))
      return -1;

   if (fence.basep.fd < 0) {
      errno = EPROTO;
      return -1;
   }

   *fd = fence.basep.fd;
   return 0;
}

int
kbase_csf_kcpu_cqs_wait(struct kbase_csf_kcpu *kcpu, uint64_t addr, uint32_t val)
{
   if (!kcpu) {
      errno = EINVAL;
      return -1;
   }

   struct base_cqs_wait_info wait = {.addr = addr, .val = val};
   struct base_kcpu_command command = {
      .type = BASE_KCPU_COMMAND_TYPE_CQS_WAIT,
      .info.cqs_wait = {
         .objs = (uint64_t)(uintptr_t)&wait,
         .nr_objs = 1,
      },
   };

   return kbase_csf_kcpu_enqueue(kcpu, &command);
}
