#if defined(HAVE_PAN_KMOD_KBASE)

struct kbase_compute_entry {
   uint64_t commands[128];
   uint32_t size;
   uint32_t subqueue;
   uint64_t target;
};

static bool
kbase_compute_encode(struct panvk_gpu_queue *queue, uint32_t subqueue,
                       uint64_t stream, uint32_t stream_size, uint64_t heap,
                       struct kbase_compute_entry *entry)
{
   struct panvk_device *dev = to_panvk_device(queue->vk.base.device);
   const struct drm_panthor_csif_info *csif = panvk_get_csif_props(dev);
   struct panvk_kbase_csf_encode_info info = {
      .register_count = csif->cs_reg_count,
      .unpreserved_count = csif->unpreserved_cs_reg_count,
      .scoreboard_count = dev->csf.sb.count,
      .ls_slot = SB_ID(LS),
      .flush_slot = SB_ID(DEFERRED_FLUSH),
      .deferred_slot = SB_ID(DEFERRED_SYNC),
      .iterator_slot = SB_ITER(0),
      .iterator_mask = dev->csf.sb.all_iters_mask,
      .context_register = PANVK_CS_REG_SUBQUEUE_CTX_START,
      .resource_mask = get_resource_mask(subqueue),
      .context_address = panvk_priv_mem_dev_addr(queue->subqueues[subqueue].context),
      .heap_address = subqueue == PANVK_SUBQUEUE_COMPUTE ? 0 : heap,
      .stream_address = stream,
      .stream_size = stream_size,
      .flush_id = panvk_get_flush_id(dev),
      .seqno_address = kbase_csf_queue_seqno_va(queue->kbase.queues[subqueue]),
   };
   /* Probe writeback: opt-in through PANVK_DEBUG=alias_writeback so the
    * production stream is byte-identical unless someone asks for it.
    * addr.dev is the aliased ring window base, so +RENDER_DESC_RINGBUF_SIZE
    * lands in the second alias entry, which is the extent no GPU access has
    * exercised yet.
    */
   if (PANVK_DEBUG(ALIAS_WRITEBACK) && queue->render_desc_ringbuf.addr.dev &&
       subqueue == PANVK_SUBQUEUE_COMPUTE) {
      info.probe_address = queue->render_desc_ringbuf.addr.dev +
                           RENDER_DESC_RINGBUF_SIZE;
      info.probe_value = 0x1122334455667788ull;
   }
   entry->subqueue = subqueue;
   return panvk_per_arch(kbase_csf_encode)(&info, entry->commands,
                                           sizeof(entry->commands), &entry->size);
}

static VkResult
kbase_compute_execute(struct panvk_gpu_queue *queue,
                        const struct kbase_compute_entry *entries, uint32_t count)
{
   struct kbase_csf_submit_entry submit[PANVK_SUBQUEUE_COUNT];
   struct kbase_csf_queue *queues[PANVK_SUBQUEUE_COUNT];
   uint64_t targets[PANVK_SUBQUEUE_COUNT];
   if (!count || count > PANVK_SUBQUEUE_COUNT)
      return VK_ERROR_UNKNOWN;
   for (uint32_t i = 0; i < count; i++) {
      uint32_t sq = entries[i].subqueue;
      queues[i] = queue->kbase.queues[sq];
      targets[i] = entries[i].target;
      submit[i] = (struct kbase_csf_submit_entry){
         .queue = queues[i], .commands = entries[i].commands,
         .size = entries[i].size, .target = targets[i],
      };
   }
   if (kbase_csf_submit(submit, count))
      return vk_queue_set_lost(&queue->vk, "kbase submission failed");
   for (uint32_t i = 0; i < count; i++)
      queue->kbase.targets[entries[i].subqueue] = targets[i];
   uint64_t deadline = os_time_get_nano() + 5000000000ull;
   if (kbase_csf_wait(queues, count, targets, deadline) != 1)
      return vk_queue_set_lost(&queue->vk, "kbase completion failed");

   /* Probe readback: with PANVK_DEBUG=alias_writeback the compute stream also
    * wrote a known value into the second alias entry.  The alias repeats one
    * backing BO, so that write lands at the start of the backing mapping and
    * can be compared here, which is the only place that sees both the alias
    * GPU address and the backing CPU mapping.
    */
   return VK_SUCCESS;
}

static bool
kbase_compute_cleanup(struct panvk_gpu_queue *queue)
{
   struct panvk_device *dev = to_panvk_device(queue->vk.base.device);
   bool stopped = true;
   for (unsigned i = 0; i < PANVK_SUBQUEUE_COUNT; i++) {
      if (queue->kbase.queues[i] && kbase_csf_queue_stop(queue->kbase.queues[i]))
         stopped = false;
   }
   if (!stopped) {
      dev->kbase.quarantined = true;
      return false;
   }
   if (queue->kbase.retired_heap && kbase_kmod_heap_destroy(queue->kbase.retired_heap)) {
      dev->kbase.quarantined = true;
      return false;
   }
   queue->kbase.retired_heap = NULL;
   if (queue->kbase.heap && kbase_kmod_heap_destroy(queue->kbase.heap)) {
      dev->kbase.quarantined = true;
      return false;
   }
   queue->kbase.heap = NULL;
   if (queue->kbase.desc_alias && kbase_kmod_alias_destroy(queue->kbase.desc_alias)) {
      dev->kbase.quarantined = true;
      return false;
   }
   queue->kbase.desc_alias = NULL;
   for (unsigned i = 0; i < PANVK_SUBQUEUE_COUNT; i++) {
      if (queue->kbase.queues[i]) {
         kbase_csf_queue_unref(queue->kbase.queues[i]);
         queue->kbase.queues[i] = NULL;
      }
      panvk_pool_free_mem(&queue->subqueues[i].context);
      panvk_pool_free_mem(&queue->subqueues[i].regs_save);
   }
   panvk_pool_free_mem(&queue->syncobjs);
   panvk_pool_free_mem(&queue->render_desc_ringbuf.syncobj);
   if (queue->render_desc_ringbuf.bo) {
      pan_kmod_bo_put(queue->render_desc_ringbuf.bo);
      queue->render_desc_ringbuf.bo = NULL;
   }
   panvk_pool_free_mem(&queue->tiler_heap.desc);
   panvk_pool_free_mem(&queue->tiler_heap.oom_fbd);
   if (queue->kbase.init_stream) {
      panvk_priv_bo_unref(queue->kbase.init_stream);
      queue->kbase.init_stream = NULL;
   }
   return true;
}

static struct kbase_kmod_heap *
kbase_graphics_heap_create(struct panvk_gpu_queue *queue)
{
   struct panvk_device *dev = to_panvk_device(queue->vk.base.device);
   return kbase_kmod_heap_create(dev->kmod.dev, 1024 * 1024, 10, 400,
                                 UINT16_MAX, 0);
}

static void
kbase_graphics_heap_desc(struct panvk_gpu_queue *queue, struct kbase_kmod_heap *heap)
{
   queue->tiler_heap.context.dev_addr = kbase_kmod_heap_context(heap);
   queue->tiler_heap.chunk_size = 1024 * 1024;
   panvk_priv_mem_write_desc(queue->tiler_heap.desc, 0, TILER_HEAP, cfg) {
      cfg.size = queue->tiler_heap.chunk_size;
      cfg.base = kbase_kmod_heap_first_chunk(heap);
      cfg.bottom = cfg.base + 64;
      cfg.top = cfg.base + cfg.size;
   }
}

static VkResult
kbase_graphics_resources(struct panvk_gpu_queue *queue)
{
   struct panvk_device *dev = to_panvk_device(queue->vk.base.device);
   if (kbase_kmod_jit_init(dev->kmod.dev, 1ull << 25, 1ull << 25, 255))
      return VK_ERROR_INITIALIZATION_FAILED;
   queue->kbase.heap = kbase_graphics_heap_create(queue);
   if (!queue->kbase.heap)
      return VK_ERROR_OUT_OF_DEVICE_MEMORY;
   struct panvk_pool_alloc_info alloc = {.size = 4096 + 65536, .alignment = 4096};
   queue->tiler_heap.desc = panvk_pool_alloc_mem(&dev->mempools.rw, alloc);
   if (!panvk_priv_mem_check_alloc(queue->tiler_heap.desc))
      return panvk_catch_indirect_alloc_failure(VK_ERROR_OUT_OF_DEVICE_MEMORY);
   kbase_graphics_heap_desc(queue, queue->kbase.heap);
   struct panvk_desc_ringbuf *ring = &queue->render_desc_ringbuf;
   ring->size = RENDER_DESC_RINGBUF_SIZE;
   ring->bo = pan_kmod_bo_alloc(dev->kmod.dev, dev->kmod.vm, ring->size, 0);
   if (!ring->bo)
      return VK_ERROR_OUT_OF_DEVICE_MEMORY;
   queue->kbase.desc_alias = kbase_kmod_alias_create(ring->bo, ring->size, 2);
   if (!queue->kbase.desc_alias)
      return VK_ERROR_OUT_OF_DEVICE_MEMORY;
   ring->addr.dev = kbase_kmod_alias_gpu_va(queue->kbase.desc_alias);
   alloc = (struct panvk_pool_alloc_info){.size = sizeof(struct panvk_cs_sync32),
                                          .alignment = 64};
   ring->syncobj = panvk_pool_alloc_mem(&dev->mempools.rw, alloc);
   if (!panvk_priv_mem_check_alloc(ring->syncobj))
      return panvk_catch_indirect_alloc_failure(VK_ERROR_OUT_OF_DEVICE_MEMORY);
   panvk_priv_mem_write(ring->syncobj, 0, struct panvk_cs_sync32, syncobj) {
      *syncobj = (struct panvk_cs_sync32){.seqno = RENDER_DESC_RINGBUF_SIZE};
   }
   queue->kbase.graphics_ready = true;
   return VK_SUCCESS;
}

static VkResult
kbase_compute_init(struct panvk_gpu_queue *queue)
{
   struct panvk_device *dev = to_panvk_device(queue->vk.base.device);
   if (PANVK_DEBUG(TRACE))
      return VK_ERROR_FEATURE_NOT_PRESENT;
   if (kbase_kmod_jit_init(dev->kmod.dev, 1ull << 25, 1ull << 25, 255))
      return VK_ERROR_INITIALIZATION_FAILED;
   for (unsigned i = 0; i < PANVK_SUBQUEUE_COUNT; i++) {
      queue->kbase.queues[i] = kbase_csf_queue_create(dev->kbase.ctx, 65536, 1);
      if (!queue->kbase.queues[i])
         return VK_ERROR_INITIALIZATION_FAILED;
   }
   VkResult result = kbase_graphics_resources(queue);
   if (result != VK_SUCCESS)
      return result;
   struct panvk_pool_alloc_info alloc = {
      .size = ALIGN_POT(sizeof(struct panvk_cs_sync64), 64) * PANVK_SUBQUEUE_COUNT,
      .alignment = 64,
   };
   queue->syncobjs = panvk_pool_alloc_mem(&dev->mempools.rw, alloc);
   if (!panvk_priv_mem_check_alloc(queue->syncobjs))
      return panvk_catch_indirect_alloc_failure(VK_ERROR_OUT_OF_DEVICE_MEMORY);
   for (unsigned i = 0; i < PANVK_SUBQUEUE_COUNT; i++) {
      panvk_priv_mem_write(queue->syncobjs, i * sizeof(struct panvk_cs_sync64),
                           struct panvk_cs_sync64, cell) {
         *cell = (struct panvk_cs_sync64){.seqno = 1};
      }
   }
   result = panvk_priv_bo_create(dev, 4096 * (PANVK_SUBQUEUE_COUNT + 1),
                                 PAN_KMOD_BO_FLAG_WB_MMAP,
                                 VK_SYSTEM_ALLOCATION_SCOPE_DEVICE,
                                 &queue->kbase.init_stream);
   if (result != VK_SUCCESS)
      return result;
   const struct drm_panthor_csif_info *csif = panvk_get_csif_props(dev);
   struct kbase_compute_entry init_entries[PANVK_SUBQUEUE_COUNT];
   for (unsigned i = 0; i < PANVK_SUBQUEUE_COUNT; i++) {
      struct panvk_subqueue *subq = &queue->subqueues[i];
      if (dev->dump_region_size[i]) {
         alloc.size = dev->dump_region_size[i];
         subq->regs_save = panvk_pool_alloc_mem(&dev->mempools.rw, alloc);
         if (!panvk_priv_mem_check_alloc(subq->regs_save))
            return panvk_catch_indirect_alloc_failure(VK_ERROR_OUT_OF_DEVICE_MEMORY);
      }
      alloc.size = sizeof(struct panvk_cs_subqueue_context);
      subq->context = panvk_pool_alloc_mem(&dev->mempools.rw, alloc);
      if (!panvk_priv_mem_check_alloc(subq->context))
         return panvk_catch_indirect_alloc_failure(VK_ERROR_OUT_OF_DEVICE_MEMORY);
      panvk_priv_mem_write(subq->context, 0, struct panvk_cs_subqueue_context, ctx) {
         *ctx = (struct panvk_cs_subqueue_context){
            .syncobjs = panvk_priv_mem_dev_addr(queue->syncobjs),
            .reg_dump_addr = panvk_priv_mem_dev_addr(subq->regs_save),
#if PAN_ARCH == 10
            .iter_sb = SB_ITER(0),
#endif
         };
         if (i != PANVK_SUBQUEUE_COMPUTE) {
            ctx->render.tiler_heap = panvk_priv_mem_dev_addr(queue->tiler_heap.desc);
            ctx->render.geom_buf = (ctx->render.tiler_heap + 4096) | 16;
            ctx->render.desc_ringbuf = (struct panvk_cs_desc_ringbuf){
               .syncobj = panvk_priv_mem_dev_addr(queue->render_desc_ringbuf.syncobj),
               .ptr = queue->render_desc_ringbuf.addr.dev,
            };
         }
      }
      struct cs_builder_conf conf = {.nr_registers = csif->cs_reg_count,
                                     .nr_kernel_registers = csif->unpreserved_cs_reg_count,
                                     .ls_sb_slot = SB_ID(LS)};
      struct cs_buffer buffer = {
         .cpu = (uint64_t *)((uint8_t *)queue->kbase.init_stream->addr.host + i * 4096),
         .gpu = queue->kbase.init_stream->addr.dev + i * 4096,
         .capacity = 4096 / sizeof(uint64_t),
      };
      struct cs_builder b;
      cs_builder_init(&b, &conf, buffer);
      cs_move64_to(&b, cs_subqueue_ctx_reg(&b), panvk_priv_mem_dev_addr(subq->context));
      for (unsigned j = 0; j < PANVK_SUBQUEUE_COUNT; j++)
         cs_move64_to(&b, cs_progress_seqno_reg(&b, j), 0);
      cs_end(&b);
      queue->kbase.init_size[i] = cs_is_valid(&b) ? cs_root_chunk_size(&b) : 0;
      cs_builder_fini(&b);
      if (!queue->kbase.init_size[i])
         return VK_ERROR_INITIALIZATION_FAILED;
      panvk_priv_bo_flush(queue->kbase.init_stream, i * 4096, queue->kbase.init_size[i]);
      init_entries[i].target = 1;
      if (!kbase_compute_encode(queue, i, buffer.gpu, queue->kbase.init_size[i],
                                 queue->tiler_heap.context.dev_addr, &init_entries[i]))
         return VK_ERROR_INITIALIZATION_FAILED;
   }
   memset((uint8_t *)queue->kbase.init_stream->addr.host +
             PANVK_SUBQUEUE_COUNT * 4096, 0, sizeof(uint64_t));
   panvk_priv_bo_flush(queue->kbase.init_stream, PANVK_SUBQUEUE_COUNT * 4096,
                        sizeof(uint64_t));
   pan_kmod_flush_bo_map_syncs(dev->kmod.dev);
   return kbase_compute_execute(queue, init_entries, PANVK_SUBQUEUE_COUNT);
}

static VkResult
kbase_compute_submit(struct panvk_gpu_queue *queue, struct vk_queue_submit *submit)
{
   struct panvk_device *dev = to_panvk_device(queue->vk.base.device);
   if (vk_queue_submit_has_bind(submit) || PANVK_DEBUG(TRACE))
      return VK_ERROR_FEATURE_NOT_PRESENT;
   for (uint32_t i = 0; i < submit->wait_count; i++) {
      if (submit->waits[i].sync->type != &panvk_kbase_sync_type || submit->waits[i].wait_value)
         return VK_ERROR_FEATURE_NOT_PRESENT;
   }
   for (uint32_t i = 0; i < submit->signal_count; i++) {
      if (submit->signals[i].sync->type != &panvk_kbase_sync_type || submit->signals[i].signal_value)
         return VK_ERROR_FEATURE_NOT_PRESENT;
   }
   if (submit->command_buffer_count > SIZE_MAX / (PANVK_SUBQUEUE_COUNT * sizeof(struct kbase_compute_entry)))
      return VK_ERROR_OUT_OF_HOST_MEMORY;
   bool graphics = false;
   uint64_t next_targets[PANVK_SUBQUEUE_COUNT];
   memcpy(next_targets, queue->kbase.targets, sizeof(next_targets));
   for (uint32_t i = 0; i < submit->command_buffer_count; i++) {
      struct panvk_cmd_buffer *cmdbuf = container_of(submit->command_buffers[i], struct panvk_cmd_buffer, vk);
      if (cmdbuf->state.contains_event_waits || cmdbuf->state.contains_timestamp_queries ||
          cmdbuf->state.uses_poly_heap)
         return VK_ERROR_FEATURE_NOT_PRESENT;
      for (unsigned j = 0; j < PANVK_SUBQUEUE_COUNT; j++) {
         struct cs_builder *b = panvk_get_cs_builder(cmdbuf, j);
         if (!cs_is_valid(b))
            return VK_ERROR_UNKNOWN;
         if (!cs_root_chunk_size(b))
            continue;
         if (next_targets[j] == UINT64_MAX)
            return VK_ERROR_UNKNOWN;
         next_targets[j]++;
         graphics |= j != PANVK_SUBQUEUE_COMPUTE;
      }
   }
   if (graphics && !queue->kbase.graphics_ready)
      return VK_ERROR_FEATURE_NOT_PRESENT;
   struct kbase_kmod_heap *replacement = NULL;
   if (graphics) {
      replacement = kbase_graphics_heap_create(queue);
      if (!replacement)
         return VK_ERROR_OUT_OF_DEVICE_MEMORY;
   }
   uint32_t wave_count = submit->command_buffer_count;
   struct kbase_compute_entry *entries = wave_count ?
      calloc((size_t)wave_count * PANVK_SUBQUEUE_COUNT, sizeof(*entries)) : NULL;
   if (wave_count && !entries) {
      if (replacement && kbase_kmod_heap_destroy(replacement)) {
         queue->kbase.retired_heap = replacement;
         dev->kbase.quarantined = true;
      }
      return VK_ERROR_OUT_OF_HOST_MEMORY;
   }
   struct kbase_compute_entry rearm[2];
   uint64_t planned[PANVK_SUBQUEUE_COUNT];
   memcpy(planned, queue->kbase.targets, sizeof(planned));
   uint64_t heap = graphics ? kbase_kmod_heap_context(replacement) :
                              queue->tiler_heap.context.dev_addr;
   VkResult result = VK_SUCCESS;
   if (graphics) {
      for (unsigned j = 0; j < 2; j++) {
         if (planned[j] == UINT64_MAX) {
            result = VK_ERROR_UNKNOWN;
            goto out;
         }
         rearm[j].target = ++planned[j];
         uint64_t noop_stream = queue->kbase.init_stream->addr.dev +
                                PANVK_SUBQUEUE_COUNT * 4096;
         if (!kbase_compute_encode(queue, j, noop_stream, sizeof(uint64_t), heap, &rearm[j])) {
            result = VK_ERROR_UNKNOWN;
            goto out;
         }
      }
   }
   for (uint32_t i = 0; i < wave_count; i++) {
      struct panvk_cmd_buffer *cmdbuf = container_of(submit->command_buffers[i], struct panvk_cmd_buffer, vk);
      for (unsigned j = 0; j < PANVK_SUBQUEUE_COUNT; j++) {
         struct cs_builder *b = panvk_get_cs_builder(cmdbuf, j);
         if (!cs_root_chunk_size(b))
            continue;
         struct kbase_compute_entry *entry = &entries[i * PANVK_SUBQUEUE_COUNT + j];
         if (planned[j] == UINT64_MAX) {
            result = VK_ERROR_UNKNOWN;
            goto out;
         }
         entry->target = ++planned[j];
         if (!kbase_compute_encode(queue, j, cs_root_chunk_gpu_addr(b), cs_root_chunk_size(b), heap, entry)) {
            result = VK_ERROR_UNKNOWN;
            goto out;
         }
      }
   }
   result = vk_sync_wait_many(&dev->vk, submit->wait_count, submit->waits,
                              VK_SYNC_WAIT_COMPLETE, UINT64_MAX);
   if (result != VK_SUCCESS)
      goto out;
   for (uint32_t i = 0; i < submit->wait_count; i++) {
      result = vk_sync_reset(&dev->vk, submit->waits[i].sync);
      if (result != VK_SUCCESS) {
         result = vk_queue_set_lost(&queue->vk, "kbase wait consumption failed");
         goto out;
      }
   }
   if (graphics) {
      struct kbase_kmod_heap *old = queue->kbase.heap;
      queue->kbase.retired_heap = old;
      queue->kbase.heap = replacement;
      replacement = NULL;
      kbase_graphics_heap_desc(queue, queue->kbase.heap);
      pan_kmod_flush_bo_map_syncs(dev->kmod.dev);
      result = kbase_compute_execute(queue, rearm, 2);
      if (result != VK_SUCCESS)
         goto out;
      if (kbase_kmod_heap_destroy(old)) {
         dev->kbase.quarantined = true;
         result = vk_queue_set_lost(&queue->vk, "kbase old heap retirement failed");
         goto out;
      }
      queue->kbase.retired_heap = NULL;
   }
   pan_kmod_flush_bo_map_syncs(dev->kmod.dev);
   for (uint32_t i = 0; i < wave_count; i++) {
      struct kbase_compute_entry wave[PANVK_SUBQUEUE_COUNT];
      uint32_t count = 0;
      for (unsigned j = 0; j < PANVK_SUBQUEUE_COUNT; j++) {
         struct kbase_compute_entry *entry = &entries[i * PANVK_SUBQUEUE_COUNT + j];
         if (entry->size)
            wave[count++] = *entry;
      }
      if (!count)
         continue;
      result = kbase_compute_execute(queue, wave, count);
      if (result != VK_SUCCESS)
         goto out;
   }
   for (uint32_t i = 0; i < submit->signal_count; i++) {
      result = vk_sync_signal(&dev->vk, submit->signals[i].sync, 0);
      if (result != VK_SUCCESS) {
         result = vk_queue_set_lost(&queue->vk, "kbase signal failed");
         break;
      }
   }
out:
   free(entries);
   if (replacement && kbase_kmod_heap_destroy(replacement)) {
      queue->kbase.retired_heap = replacement;
      dev->kbase.quarantined = true;
   }
   return result;
}
#endif
