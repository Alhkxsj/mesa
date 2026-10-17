#include "panvk_kbase_device.h"
#include "panvk_instance.h"
#include "panvk_physical_device.h"
#include "panvk_kbase_sync.h"
#include "kmod/kbase_kmod.h"
#include "vk_alloc.h"

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <unistd.h>

static bool
valid_device_path(const char *path)
{
   static const char prefix[] = "/dev/mali";
   if (strncmp(path, prefix, sizeof(prefix) - 1) ||
       strlen(path) >= PANVK_KBASE_PATH_SIZE)
      return false;
   const char *suffix = path + sizeof(prefix) - 1;
   if (!*suffix)
      return false;
   for (; *suffix; suffix++)
      if (*suffix < '0' || *suffix > '9')
         return false;
   return true;
}

enum panvk_kbase_selection
panvk_kbase_select_device(const char *path, const char *restriction,
                          char selected[PANVK_KBASE_PATH_SIZE])
{
   selected[0] = '\0';
   if (!path)
      return PANVK_KBASE_SELECTION_DEFAULT;
   if (!restriction || strcmp(restriction, "kbase") || !valid_device_path(path))
      return PANVK_KBASE_SELECTION_INVALID;
   memcpy(selected, path, strlen(path) + 1);
   return PANVK_KBASE_SELECTION_EXPLICIT;
}

VkResult
panvk_kbase_open_device(const char *path, const struct pan_kmod_allocator *allocator,
                        struct pan_kmod_dev **out)
{
   *out = NULL;
   if (!path || !valid_device_path(path))
      return VK_ERROR_INCOMPATIBLE_DRIVER;
   int fd = open(path, O_RDWR | O_CLOEXEC);
   if (fd < 0)
      return VK_ERROR_INCOMPATIBLE_DRIVER;
   struct pan_kmod_dev *dev = kbase_kmod_ops.dev_create(
      fd, PAN_KMOD_DEV_FLAG_OWNS_FD, allocator);
   if (!dev) {
      int error = errno;
      close(fd);
      errno = error;
      return panvk_catch_indirect_alloc_failure(VK_ERROR_INCOMPATIBLE_DRIVER);
   }
   if (!kbase_kmod_supports_panvk(dev)) {
      pan_kmod_dev_destroy(dev);
      return VK_ERROR_INCOMPATIBLE_DRIVER;
   }
   *out = dev;
   return VK_SUCCESS;
}

VkResult
panvk_kbase_enumerate_physical_device(struct panvk_instance *instance)
{
   if (instance->kbase.selection != PANVK_KBASE_SELECTION_EXPLICIT)
      return VK_SUCCESS;
   struct panvk_physical_device *device = vk_zalloc(
      &instance->vk.alloc, sizeof(*device), 8, VK_SYSTEM_ALLOCATION_SCOPE_INSTANCE);
   if (!device)
      return VK_ERROR_OUT_OF_HOST_MEMORY;
   VkResult result = panvk_physical_device_init_kbase(device, instance,
                                                     instance->kbase.path);
   if (result != VK_SUCCESS) {
      vk_free(&instance->vk.alloc, device);
      return result == VK_ERROR_INCOMPATIBLE_DRIVER ? VK_SUCCESS : result;
   }
   list_addtail(&device->vk.link, &instance->vk.physical_devices.list);
   return VK_SUCCESS;
}

void
panvk_kbase_physical_device_capabilities(struct vk_device_extension_table *extensions,
                                        struct vk_features *features)
{
   if (extensions)
      *extensions = (struct vk_device_extension_table){0};
   if (features)
      *features = (struct vk_features){0};
}

void
panvk_kbase_physical_device_setup(struct panvk_physical_device *device)
{
   assert(panvk_physical_device_uses_kbase(device));
   memset(device->sync_types, 0, sizeof(device->sync_types));
   device->sync_types[0] = &panvk_kbase_sync_type;
   device->vk.supported_sync_types = device->sync_types;
}

void
panvk_kbase_physical_device_properties(struct vk_properties *properties)
{
   properties->apiVersion = VK_API_VERSION_1_0;
   properties->conformanceVersion = (VkConformanceVersion){0};
   properties->timestampComputeAndGraphics = false;
   properties->timestampPeriod = 1.0f;
}
