#ifndef KBASE_CSF_H
#define KBASE_CSF_H

#include "pan_kmod.h"

struct drm_panthor_csif_info;

struct kbase_csf_context;
struct kbase_csf_queue;

struct kbase_csf_submit_entry {
   struct kbase_csf_queue *queue;
   const void *commands;
   uint32_t size;
   uint64_t target;
};

struct kbase_csf_caps {
   uint32_t version;
   uint32_t features;
   uint32_t group_count;
   uint32_t streams_per_group;
   uint32_t stream_features;
   uint32_t instr_features;
};

struct kbase_csf_context *kbase_csf_context_create(struct pan_kmod_dev *dev);
void kbase_csf_context_ref(struct kbase_csf_context *ctx);
void kbase_csf_context_unref(struct kbase_csf_context *ctx);
void kbase_csf_context_cancel(struct kbase_csf_context *ctx);
const struct kbase_csf_caps *kbase_csf_context_caps(struct kbase_csf_context *ctx);
uint32_t kbase_csf_context_flush_id(struct kbase_csf_context *ctx);
struct pan_kmod_dev *kbase_csf_context_dev(struct kbase_csf_context *ctx);
struct pan_kmod_vm *kbase_csf_context_vm(struct kbase_csf_context *ctx);
int kbase_csf_context_get_csif(struct kbase_csf_context *ctx,
                               struct drm_panthor_csif_info *out);
int kbase_csf_context_status(struct kbase_csf_context *ctx);

struct kbase_csf_queue *kbase_csf_queue_create(struct kbase_csf_context *ctx,
                                               uint32_t ring_size,
                                               uint8_t priority);
void kbase_csf_queue_ref(struct kbase_csf_queue *queue);
void kbase_csf_queue_unref(struct kbase_csf_queue *queue);
void kbase_csf_queue_cancel(struct kbase_csf_queue *queue);
int kbase_csf_queue_stop(struct kbase_csf_queue *queue);
uint64_t kbase_csf_queue_seqno_va(struct kbase_csf_queue *queue);
uint64_t kbase_csf_queue_published_target(struct kbase_csf_queue *queue);
int kbase_csf_queue_prepare(struct kbase_csf_queue *queue, const void *commands,
                            uint32_t size, uint64_t target);
int kbase_csf_publish(struct kbase_csf_queue *const *queues, uint32_t count);
int kbase_csf_submit(const struct kbase_csf_submit_entry *entries, uint32_t count);
int kbase_csf_wait(struct kbase_csf_queue *const *queues, uint32_t count,
                   const uint64_t *targets, uint64_t abs_timeout_ns);

#endif
