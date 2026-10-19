/*
 * Copyright © 2021 Collabora Ltd.
 * Copyright © 2025 Arm Ltd.
 *
 * Derived from tu_wsi.c:
 * Copyright © 2016 Red Hat
 * Copyright © 2015 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 */

#include <stdlib.h>

#include "panvk_wsi.h"
#include "panvk_instance.h"
#include "panvk_physical_device.h"

#include "vk_util.h"
#include "wsi_common.h"

static VKAPI_PTR PFN_vkVoidFunction
panvk_wsi_proc_addr(VkPhysicalDevice physicalDevice, const char *pName)
{
   VK_FROM_HANDLE(panvk_physical_device, pdevice, physicalDevice);
   struct panvk_instance *instance = to_panvk_instance(pdevice->vk.instance);

   return vk_instance_get_proc_addr_unchecked(&instance->vk, pName);
}

static bool
panvk_can_present_on_device(VkPhysicalDevice pdevice, int fd)
{
   drmDevicePtr device;
   if (drmGetDevice2(fd, 0, &device) != 0)
      return false;
   /* Allow on-device presentation for all devices with bus type PLATFORM.
    * Other device types such as PCI or USB should use the PRIME blit path. */
   bool match = device->bustype == DRM_BUS_PLATFORM;

   drmFreeDevice(&device);

   return match;
}

VkResult
panvk_wsi_init(struct panvk_physical_device *physical_device)
{
   struct panvk_instance *instance =
      to_panvk_instance(physical_device->vk.instance);
   VkResult result;

   /* Software presentation is opt-in via PANVK_KBASE_WSI_SW.
    * PANVK_KBASE_WSI_NATIVE selects the native X11/DRI3 path instead, which
    * relies on the dma-heap export path. */
   const bool sw_device =
      getenv("PANVK_KBASE_WSI_SW") != NULL &&
      getenv("PANVK_KBASE_WSI_NATIVE") == NULL;

   /* Termux:X11 and Winlator present raw dma-buf FDs through the DRI3
    * PixmapFromBuffers request with a private modifier (1274) and do not
    * support DRI3 fences on that path.  PANVK_KBASE_DRI3=raw/termux and the
    * WSI_X11_TERMUX=1 switch select it, matching the earlier mesa-mtk fork. */
   const char *dri3_option = getenv("PANVK_KBASE_DRI3");
   const char *termux_wsi = getenv("WSI_X11_TERMUX");
   const bool termux_raw_dri3 =
      !sw_device && !dri3_option && termux_wsi && strcmp(termux_wsi, "0") != 0;
   const bool raw_fd_dri3 =
      !sw_device &&
      (termux_raw_dri3 ||
       (dri3_option &&
        (!strcmp(dri3_option, "raw") || !strcmp(dri3_option, "termux"))));

   result = wsi_device_init(&physical_device->wsi_device,
                            panvk_physical_device_to_handle(physical_device),
                            panvk_wsi_proc_addr, &instance->vk.alloc, -1,
                            &instance->drirc.options,
                            &(struct wsi_device_options){
                               .sw_device = sw_device,
                               /* kbase cannot attach a GPU completion fence to
                                * the exported dma-buf, so wait for rendering
                                * before queueing the image for presentation. */
                               .wait_present_before_queue = !sw_device,
                               .x11_use_raw_fd_modifier = raw_fd_dri3,
                            });
   if (result != VK_SUCCESS)
      return result;

   /* kbase sync state lives in userspace seqnos and cannot be copied as DRM
    * syncobj payloads. Keep even empty WSI submits on the real queue so
    * present fences are backed by the kbase submission timeline. */
   physical_device->wsi_device.disable_unordered_submits =
      panvk_physical_device_uses_kbase(physical_device);

   /* Native kbase WSI uses linear dma-bufs until DRM format modifiers are
    * verified; modifiers would require VK_EXT_image_drm_format_modifier. */
   physical_device->wsi_device.supports_modifiers =
      !panvk_physical_device_uses_kbase(physical_device);
   physical_device->wsi_device.can_present_on_device =
      panvk_can_present_on_device;

   physical_device->vk.wsi_device = &physical_device->wsi_device;

   return VK_SUCCESS;
}

void
panvk_wsi_finish(struct panvk_physical_device *physical_device)
{
   struct panvk_instance *instance =
      to_panvk_instance(physical_device->vk.instance);

   physical_device->vk.wsi_device = NULL;
   wsi_device_finish(&physical_device->wsi_device, &instance->vk.alloc);
}
