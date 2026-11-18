/* SPDX-License-Identifier: MIT-0 */
/* exports vkGetInstanceProcAddr over KosmicKrisp's ICD entry, so ash::Entry::load_from can open it as a loader */
typedef void (*PFN)(void);
extern PFN vk_icdGetInstanceProcAddr(void *instance, const char *name);
PFN vkGetInstanceProcAddr(void *instance, const char *name) { return vk_icdGetInstanceProcAddr(instance, name); }
