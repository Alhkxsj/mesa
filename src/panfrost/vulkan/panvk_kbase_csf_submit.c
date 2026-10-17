#include "panvk_kbase_csf_submit.h"
#include "panvk_kbase_csf_sync.h"
#include "panvk_kbase_sync.h"

#include <errno.h>
#include <stdlib.h>

#include "kmod/kbase_csf.h"

#define KBASE_SUBMIT_MAX_QUEUES 64
#define KBASE_WRAPPER_CAPACITY 1024

VkResult
panvk_kbase_csf_submit(const struct panvk_kbase_csf_submit_item *items,
                        uint32_t count, struct vk_sync *const *signals,
                        uint32_t signal_count)
{
   if (!items || !count || count > KBASE_SUBMIT_MAX_QUEUES ||
       signal_count > KBASE_SUBMIT_MAX_QUEUES || (signal_count && !signals))
      return VK_ERROR_UNKNOWN;
   for (uint32_t i = 0; i < signal_count; i++) {
      if (!signals[i] || signals[i]->type != &panvk_kbase_sync_type)
         return VK_ERROR_UNKNOWN;
      for (uint32_t j = 0; j < i; j++) {
         if (signals[i] == signals[j])
            return VK_ERROR_UNKNOWN;
      }
   }
   for (uint32_t i = 0; i < count; i++) {
      if (!items[i].queue || !items[i].encode || !items[i].target ||
          items[i].info.seqno_address != kbase_csf_queue_seqno_va(items[i].queue))
         return VK_ERROR_UNKNOWN;
      for (uint32_t j = 0; j < i; j++) {
         if (items[i].queue == items[j].queue)
            return VK_ERROR_UNKNOWN;
      }
   }

   uint8_t *storage = calloc(count, KBASE_WRAPPER_CAPACITY);
   if (!storage)
      return VK_ERROR_OUT_OF_HOST_MEMORY;
   struct kbase_csf_submit_entry entries[KBASE_SUBMIT_MAX_QUEUES];
   struct kbase_csf_queue *queues[KBASE_SUBMIT_MAX_QUEUES];
   uint64_t targets[KBASE_SUBMIT_MAX_QUEUES];
   struct panvk_kbase_completion *completions[KBASE_SUBMIT_MAX_QUEUES] = {0};
   VkResult result = VK_SUCCESS;

   for (uint32_t i = 0; i < count; i++) {
      uint32_t size = 0;
      void *commands = storage + i * KBASE_WRAPPER_CAPACITY;
      if (!items[i].encode(&items[i].info, commands, KBASE_WRAPPER_CAPACITY, &size) ||
          !size || size > KBASE_WRAPPER_CAPACITY) {
         result = VK_ERROR_UNKNOWN;
         goto out;
      }
      queues[i] = items[i].queue;
      targets[i] = items[i].target;
      entries[i] = (struct kbase_csf_submit_entry){
         .queue = queues[i],
         .commands = commands,
         .size = size,
         .target = targets[i],
      };
   }

   if (signal_count) {
      result = panvk_kbase_csf_sync_prepare(queues, count, targets, &completions[0]);
      if (result != VK_SUCCESS)
         goto out;
      for (uint32_t i = 1; i < signal_count; i++) {
         panvk_kbase_completion_ref(completions[0]);
         completions[i] = completions[0];
      }
   }
   if (kbase_csf_submit(entries, count)) {
      result = errno == ENOMEM ? VK_ERROR_OUT_OF_HOST_MEMORY :
               errno == EAGAIN ? VK_NOT_READY :
               (errno == EINVAL || errno == EOVERFLOW) ? VK_ERROR_UNKNOWN :
                                                       VK_ERROR_DEVICE_LOST;
      goto out;
   }
   if (signal_count)
      panvk_kbase_completion_commit_many(signal_count, signals, completions);

out:
   for (uint32_t i = 0; i < signal_count; i++)
      panvk_kbase_completion_unref(completions[i]);
   free(storage);
   return result;
}
