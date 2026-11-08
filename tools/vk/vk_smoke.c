/*
 * vk_smoke: the first thing to run on a Vulkan driver of ours, with no loader in between. It takes
 * the driver's entry (vk_icdGetInstanceProcAddr) and goes one step at a time, saying on one line
 * what each step got, so that a log read afterwards shows how far the driver went:
 *
 *   1 instance      vkCreateInstance (the API version asked: VK_SMOKE_API=1.0|1.1|1.2|1.3, default 1.0)
 *   2 device        the physical devices; name, API version, driver version, vendor and device IDs
 *   3 extensions    the count, and whether each extension a Metal-on-Vulkan runtime leans on is there
 *   4 features      Vulkan 1.2's features where the device says 1.2 or more, else the extensions' own
 *   5 logical       vkCreateDevice with one queue of a family that has graphics and compute
 *   6 clear         a 64x64 RGBA8 image cleared on the GPU, copied into a buffer, read back
 *   7 timeline      a timeline semaphore signalled by a submit and waited on by the CPU
 *   8 compute       a compute shader (SPIR-V below) writing 2i+1 into 4096 words, read back
 *
 * The program links against the driver (tools/mk-panvk.sh builds both) or takes VK_SMOKE_DRIVER=PATH
 * (or a path as its last argument) and dlopen()s it. It never needs a window or a display. The last
 * line says how many steps passed. Arguments, for launchd jobs that cannot set the environment:
 *   --mali-v6   set PAN_I_WANT_A_BROKEN_VULKAN_DRIVER=1 first: PanVK refuses a Bifrost v6 GPU (the
 *               S9's Mali-G72) without it ("not well-tested on v6"); a test is what it is for
 *   --api X.Y   the API version asked for (as VK_SMOKE_API)
 *
 * Copyright (c) 2026 Scion contributors. MIT licence (see the repository's LICENSE).
 */
#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

static PFN_vkGetInstanceProcAddr gipa;
static int passed, total, unchecked;
/* HAKIOS_DRM_NOOP=1 (tools/mesa/hakios-drm): a made-up GPU that runs no job, so nothing is ever written back */
static int noop;

#define STEP(n, name) do { total++; printf("vk_smoke: %d %s: ", (n), (name)); } while (0)
#define OK(...) do { passed++; printf("ok, " __VA_ARGS__); printf("\n"); fflush(stdout); } while (0)
#define FAIL(...) do { printf("FAILED, " __VA_ARGS__); printf("\n"); fflush(stdout); } while (0)

/*
 * The compute shader, SPIR-V 1.0 written out by hand (no compiler is needed to build this file):
 *
 *   OpCapability Shader
 *   OpMemoryModel Logical GLSL450
 *   OpEntryPoint GLCompute %main "main" %gid
 *   OpExecutionMode %main LocalSize 64 1 1
 *   OpDecorate %gid BuiltIn GlobalInvocationId
 *   OpDecorate %rta ArrayStride 4
 *   OpMemberDecorate %blk 0 Offset 0
 *   OpDecorate %blk BufferBlock
 *   OpDecorate %buf DescriptorSet 0
 *   OpDecorate %buf Binding 0
 *   ... %buf: Uniform pointer to struct { uint[] }
 *   main: x = gid.x; buf[x] = x * 2 + 1
 */
static const uint32_t shader[] = {
	0x07230203, 0x00010000, 0x00000000, 0x00000016, 0x00000000, 0x00020011,
	0x00000001, 0x0003000e, 0x00000000, 0x00000001, 0x0006000f, 0x00000005,
	0x00000001, 0x6e69616d, 0x00000000, 0x00000002, 0x00060010, 0x00000001,
	0x00000011, 0x00000040, 0x00000001, 0x00000001, 0x00040047, 0x00000002,
	0x0000000b, 0x0000001c, 0x00040047, 0x00000003, 0x00000006, 0x00000004,
	0x00050048, 0x00000004, 0x00000000, 0x00000023, 0x00000000, 0x00030047,
	0x00000004, 0x00000003, 0x00040047, 0x00000005, 0x00000022, 0x00000000,
	0x00040047, 0x00000005, 0x00000021, 0x00000000, 0x00020013, 0x00000006,
	0x00030021, 0x00000007, 0x00000006, 0x00040015, 0x00000008, 0x00000020,
	0x00000000, 0x00040017, 0x00000009, 0x00000008, 0x00000003, 0x00040020,
	0x0000000a, 0x00000001, 0x00000009, 0x0004003b, 0x0000000a, 0x00000002,
	0x00000001, 0x0003001d, 0x00000003, 0x00000008, 0x0003001e, 0x00000004,
	0x00000003, 0x00040020, 0x0000000b, 0x00000002, 0x00000004, 0x0004003b,
	0x0000000b, 0x00000005, 0x00000002, 0x00040020, 0x0000000c, 0x00000002,
	0x00000008, 0x0004002b, 0x00000008, 0x0000000d, 0x00000000, 0x0004002b,
	0x00000008, 0x0000000e, 0x00000001, 0x0004002b, 0x00000008, 0x0000000f,
	0x00000002, 0x00050036, 0x00000006, 0x00000001, 0x00000000, 0x00000007,
	0x000200f8, 0x00000010, 0x0004003d, 0x00000009, 0x00000011, 0x00000002,
	0x00050051, 0x00000008, 0x00000012, 0x00000011, 0x00000000, 0x00050084,
	0x00000008, 0x00000013, 0x00000012, 0x0000000f, 0x00050080, 0x00000008,
	0x00000014, 0x00000013, 0x0000000e, 0x00060041, 0x0000000c, 0x00000015,
	0x00000005, 0x0000000d, 0x00000012, 0x0003003e, 0x00000015, 0x00000014,
	0x000100fd, 0x00010038,
};

/* the extensions a Metal-on-Vulkan runtime (metal2vulkan's floor is Vulkan 1.2) asks about first */
static const char *wanted[] = {
	"VK_KHR_timeline_semaphore", "VK_KHR_buffer_device_address", "VK_KHR_synchronization2",
	"VK_KHR_dynamic_rendering", "VK_KHR_create_renderpass2", "VK_KHR_maintenance4",
	"VK_KHR_vulkan_memory_model", "VK_EXT_descriptor_indexing", "VK_KHR_8bit_storage",
	"VK_KHR_16bit_storage", "VK_KHR_shader_float16_int8", "VK_KHR_push_descriptor",
	"VK_KHR_external_memory", "VK_EXT_external_memory_host", "VK_EXT_extended_dynamic_state",
	"VK_KHR_driver_properties", "VK_EXT_scalar_block_layout", "VK_KHR_separate_depth_stencil_layouts",
};

#define GET(inst, name) PFN_##name name = (PFN_##name)gipa(inst, #name)
#define DGET(name) PFN_##name name = (PFN_##name)vkGetDeviceProcAddr(dev, #name)

static uint32_t
find_memory(const VkPhysicalDeviceMemoryProperties *mp, uint32_t bits, VkMemoryPropertyFlags want)
{
	for (uint32_t i = 0; i < mp->memoryTypeCount; i++)
		if ((bits & (1u << i)) && (mp->memoryTypes[i].propertyFlags & want) == want)
			return i;
	return UINT32_MAX;
}

int
main(int argc, char **argv)
{
	const char *path = getenv("VK_SMOKE_DRIVER");
	const char *want_api = getenv("VK_SMOKE_API");
	const char *nv = getenv("HAKIOS_DRM_NOOP");

	noop = nv != NULL && nv[0] != '\0' && strcmp(nv, "0") != 0;

	for (int a = 1; a < argc; a++) {
		if (!strcmp(argv[a], "--mali-v6"))
			setenv("PAN_I_WANT_A_BROKEN_VULKAN_DRIVER", "1", 1);
		else if (!strcmp(argv[a], "--api") && a + 1 < argc)
			want_api = argv[++a];
		else
			path = argv[a];
	}
	void *h = RTLD_DEFAULT;
	if (path) {
		h = dlopen(path, RTLD_NOW | RTLD_LOCAL);
		if (!h) {
			printf("vk_smoke: cannot open %s: %s\n", path, dlerror());
			return 2;
		}
		gipa = (PFN_vkGetInstanceProcAddr)dlsym(h, "vk_icdGetInstanceProcAddr");
		if (!gipa)
			gipa = (PFN_vkGetInstanceProcAddr)dlsym(h, "vkGetInstanceProcAddr");
	} else {
		gipa = (PFN_vkGetInstanceProcAddr)dlsym(RTLD_DEFAULT, "vk_icdGetInstanceProcAddr");
	}
	if (!gipa) {
		printf("vk_smoke: no vk_icdGetInstanceProcAddr\n");
		return 2;
	}
	/* the loader-ICD handshake, as a loader would do it */
	typedef VkResult (*negotiate_t)(uint32_t *);
	negotiate_t negotiate = (negotiate_t)dlsym(h, "vk_icdNegotiateLoaderICDInterfaceVersion");
	if (negotiate) {
		uint32_t v = 5;
		VkResult r = negotiate(&v);
		printf("vk_smoke: ICD interface: asked 5, the driver says %u (%d)\n", v, r);
	}

	uint32_t api = VK_API_VERSION_1_0;
	if (want_api && !strcmp(want_api, "1.1")) api = VK_API_VERSION_1_1;
	if (want_api && !strcmp(want_api, "1.2")) api = VK_API_VERSION_1_2;
	if (want_api && !strcmp(want_api, "1.3")) api = VK_API_VERSION_1_3;

	/* 1 */
	STEP(1, "instance");
	GET(NULL, vkCreateInstance);
	VkApplicationInfo app = { .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .pApplicationName = "vk_smoke",
		.apiVersion = api };
	/* below 1.1, vkGetPhysicalDeviceFeatures2 is VK_KHR_get_physical_device_properties2's */
	const char *inst_ext[1] = { "VK_KHR_get_physical_device_properties2" };
	VkInstanceCreateInfo ici = { .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app,
		.enabledExtensionCount = api < VK_API_VERSION_1_1 ? 1 : 0, .ppEnabledExtensionNames = inst_ext };
	VkInstance inst;
	VkResult r = vkCreateInstance ? vkCreateInstance(&ici, NULL, &inst) : VK_ERROR_INITIALIZATION_FAILED;
	if (r != VK_SUCCESS) {
		FAIL("vkCreateInstance %d", r);
		goto done;
	}
	OK("API %u.%u asked", VK_API_VERSION_MAJOR(api), VK_API_VERSION_MINOR(api));

	/* 2 */
	STEP(2, "device");
	GET(inst, vkEnumeratePhysicalDevices);
	GET(inst, vkGetPhysicalDeviceProperties);
	uint32_t n = 0;
	r = vkEnumeratePhysicalDevices(inst, &n, NULL);
	if (r != VK_SUCCESS || n == 0) {
		FAIL("%u physical devices (%d): on a Mali-G72, is PAN_I_WANT_A_BROKEN_VULKAN_DRIVER=1 set?", n, r);
		goto done;
	}
	VkPhysicalDevice pd;
	n = 1;
	vkEnumeratePhysicalDevices(inst, &n, &pd);
	VkPhysicalDeviceProperties props;
	vkGetPhysicalDeviceProperties(pd, &props);
	OK("\"%s\", Vulkan %u.%u.%u, driver %#x, vendor %#x, device %#x", props.deviceName,
	    VK_API_VERSION_MAJOR(props.apiVersion), VK_API_VERSION_MINOR(props.apiVersion),
	    VK_API_VERSION_PATCH(props.apiVersion), props.driverVersion, props.vendorID, props.deviceID);

	/* 3 */
	STEP(3, "extensions");
	GET(inst, vkEnumerateDeviceExtensionProperties);
	uint32_t ne = 0;
	vkEnumerateDeviceExtensionProperties(pd, NULL, &ne, NULL);
	VkExtensionProperties *ext = calloc(ne ? ne : 1, sizeof(*ext));
	vkEnumerateDeviceExtensionProperties(pd, NULL, &ne, ext);
	int have_timeline = 0;
	char line[2048] = "";
	for (size_t w = 0; w < sizeof(wanted) / sizeof(wanted[0]); w++) {
		int found = 0;
		for (uint32_t e = 0; e < ne; e++)
			if (!strcmp(ext[e].extensionName, wanted[w]))
				found = 1;
		if (!strcmp(wanted[w], "VK_KHR_timeline_semaphore"))
			have_timeline = found;
		snprintf(line + strlen(line), sizeof(line) - strlen(line), " %s%s", found ? "+" : "-", wanted[w] + 3);
	}
	OK("%u of them;%s", ne, line);
	if (getenv("VK_SMOKE_ALL"))
		for (uint32_t e = 0; e < ne; e++)
			printf("vk_smoke:   %s %u\n", ext[e].extensionName, ext[e].specVersion);

	/* 4 */
	STEP(4, "features");
	PFN_vkGetPhysicalDeviceFeatures2 vkGetPhysicalDeviceFeatures2 = api >= VK_API_VERSION_1_1 ?
	    (PFN_vkGetPhysicalDeviceFeatures2)gipa(inst, "vkGetPhysicalDeviceFeatures2") : NULL;
	if (!vkGetPhysicalDeviceFeatures2)
		vkGetPhysicalDeviceFeatures2 = (PFN_vkGetPhysicalDeviceFeatures2)gipa(inst, "vkGetPhysicalDeviceFeatures2KHR");
	VkPhysicalDeviceTimelineSemaphoreFeatures tsf = {
		.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES };
	VkPhysicalDeviceVulkan12Features v12 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES };
	VkPhysicalDeviceFeatures2 f2 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2 };
	int is12 = props.apiVersion >= VK_API_VERSION_1_2 && api >= VK_API_VERSION_1_2;
	f2.pNext = is12 ? (void *)&v12 : have_timeline ? (void *)&tsf : NULL;
	if (vkGetPhysicalDeviceFeatures2) {
		vkGetPhysicalDeviceFeatures2(pd, &f2);
		if (is12)
			OK("1.2: timelineSemaphore %u bufferDeviceAddress %u descriptorIndexing %u vulkanMemoryModel %u "
			    "shaderFloat16 %u shaderInt8 %u storageBuffer8BitAccess %u scalarBlockLayout %u; "
			    "1.0: shaderInt64 %u shaderInt16 %u samplerAnisotropy %u textureCompressionBC %u "
			    "fragmentStoresAndAtomics %u independentBlend %u",
			    v12.timelineSemaphore, v12.bufferDeviceAddress, v12.descriptorIndexing, v12.vulkanMemoryModel,
			    v12.shaderFloat16, v12.shaderInt8, v12.storageBuffer8BitAccess, v12.scalarBlockLayout,
			    f2.features.shaderInt64, f2.features.shaderInt16, f2.features.samplerAnisotropy,
			    f2.features.textureCompressionBC, f2.features.fragmentStoresAndAtomics,
			    f2.features.independentBlend);
		else
			OK("timelineSemaphore %u (by the extension); 1.0: shaderInt64 %u shaderInt16 %u samplerAnisotropy %u "
			    "textureCompressionBC %u fragmentStoresAndAtomics %u independentBlend %u",
			    tsf.timelineSemaphore, f2.features.shaderInt64, f2.features.shaderInt16,
			    f2.features.samplerAnisotropy, f2.features.textureCompressionBC,
			    f2.features.fragmentStoresAndAtomics, f2.features.independentBlend);
	} else
		FAIL("no vkGetPhysicalDeviceFeatures2");
	int timeline = is12 ? v12.timelineSemaphore : tsf.timelineSemaphore;

	/* 5 */
	STEP(5, "logical");
	GET(inst, vkGetPhysicalDeviceQueueFamilyProperties);
	GET(inst, vkGetPhysicalDeviceMemoryProperties);
	GET(inst, vkCreateDevice);
	GET(inst, vkGetDeviceProcAddr);
	uint32_t nq = 0;
	vkGetPhysicalDeviceQueueFamilyProperties(pd, &nq, NULL);
	VkQueueFamilyProperties qf[16];
	if (nq > 16) nq = 16;
	vkGetPhysicalDeviceQueueFamilyProperties(pd, &nq, qf);
	uint32_t family = UINT32_MAX;
	for (uint32_t q = 0; q < nq; q++)
		if ((qf[q].queueFlags & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) ==
		    (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) {
			family = q;
			break;
		}
	if (family == UINT32_MAX) {
		FAIL("no queue family with graphics and compute (%u families)", nq);
		goto done;
	}
	float prio = 1.0f;
	VkDeviceQueueCreateInfo qci = { .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
		.queueFamilyIndex = family, .queueCount = 1, .pQueuePriorities = &prio };
	const char *dev_ext[1] = { "VK_KHR_timeline_semaphore" };
	VkPhysicalDeviceTimelineSemaphoreFeatures tse = {
		.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES, .timelineSemaphore = VK_TRUE };
	VkPhysicalDeviceVulkan12Features v12e = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,
		.timelineSemaphore = VK_TRUE };
	VkDeviceCreateInfo dci = { .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .queueCreateInfoCount = 1,
		.pQueueCreateInfos = &qci };
	if (timeline) {
		if (is12)
			dci.pNext = &v12e;
		else {
			dci.pNext = &tse;
			dci.enabledExtensionCount = 1;
			dci.ppEnabledExtensionNames = dev_ext;
		}
	}
	VkDevice dev;
	r = vkCreateDevice(pd, &dci, NULL, &dev);
	if (r != VK_SUCCESS) {
		FAIL("vkCreateDevice %d", r);
		goto done;
	}
	OK("queue family %u of %u (flags %#x, %u queues), timeline semaphores %s", family, nq,
	    qf[family].queueFlags, qf[family].queueCount, timeline ? "on" : "off");

	DGET(vkGetDeviceQueue);
	DGET(vkCreateBuffer);
	DGET(vkGetBufferMemoryRequirements);
	DGET(vkAllocateMemory);
	DGET(vkBindBufferMemory);
	DGET(vkMapMemory);
	DGET(vkCreateImage);
	DGET(vkGetImageMemoryRequirements);
	DGET(vkBindImageMemory);
	DGET(vkCreateCommandPool);
	DGET(vkAllocateCommandBuffers);
	DGET(vkBeginCommandBuffer);
	DGET(vkEndCommandBuffer);
	DGET(vkCmdPipelineBarrier);
	DGET(vkCmdClearColorImage);
	DGET(vkCmdCopyImageToBuffer);
	DGET(vkQueueSubmit);
	DGET(vkQueueWaitIdle);
	DGET(vkResetCommandBuffer);
	DGET(vkCreateSemaphore);
	DGET(vkCreateShaderModule);
	DGET(vkCreateDescriptorSetLayout);
	DGET(vkCreatePipelineLayout);
	DGET(vkCreateComputePipelines);
	DGET(vkCreateDescriptorPool);
	DGET(vkAllocateDescriptorSets);
	DGET(vkUpdateDescriptorSets);
	DGET(vkCmdBindPipeline);
	DGET(vkCmdBindDescriptorSets);
	DGET(vkCmdDispatch);
	PFN_vkWaitSemaphores vkWaitSemaphores = (PFN_vkWaitSemaphores)vkGetDeviceProcAddr(dev, "vkWaitSemaphores");
	if (!vkWaitSemaphores)
		vkWaitSemaphores = (PFN_vkWaitSemaphores)vkGetDeviceProcAddr(dev, "vkWaitSemaphoresKHR");
	PFN_vkGetSemaphoreCounterValue vkGetSemaphoreCounterValue =
	    (PFN_vkGetSemaphoreCounterValue)vkGetDeviceProcAddr(dev, "vkGetSemaphoreCounterValue");
	if (!vkGetSemaphoreCounterValue)
		vkGetSemaphoreCounterValue =
		    (PFN_vkGetSemaphoreCounterValue)vkGetDeviceProcAddr(dev, "vkGetSemaphoreCounterValueKHR");

	VkQueue queue;
	vkGetDeviceQueue(dev, family, 0, &queue);
	VkPhysicalDeviceMemoryProperties mp;
	vkGetPhysicalDeviceMemoryProperties(pd, &mp);

	/* a host-visible buffer of 64 KB, for every read back */
	VkBufferCreateInfo bci = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = 65536,
		.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT };
	VkBuffer buf;
	VkMemoryRequirements mr;
	VkDeviceMemory bmem;
	uint32_t *words = NULL;
	if (vkCreateBuffer(dev, &bci, NULL, &buf) != VK_SUCCESS)
		goto done;
	vkGetBufferMemoryRequirements(dev, buf, &mr);
	VkMemoryAllocateInfo mai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size,
		.memoryTypeIndex = find_memory(&mp, mr.memoryTypeBits,
		    VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) };
	if (mai.memoryTypeIndex == UINT32_MAX || vkAllocateMemory(dev, &mai, NULL, &bmem) != VK_SUCCESS ||
	    vkBindBufferMemory(dev, buf, bmem, 0) != VK_SUCCESS ||
	    vkMapMemory(dev, bmem, 0, VK_WHOLE_SIZE, 0, (void **)&words) != VK_SUCCESS) {
		printf("vk_smoke: no host-visible, coherent buffer memory\n");
		goto done;
	}

	VkCommandPoolCreateInfo cpci = { .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
		.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, .queueFamilyIndex = family };
	VkCommandPool pool;
	vkCreateCommandPool(dev, &cpci, NULL, &pool);
	VkCommandBufferAllocateInfo cbai = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
		.commandPool = pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1 };
	VkCommandBuffer cb;
	vkAllocateCommandBuffers(dev, &cbai, &cb);
	VkCommandBufferBeginInfo cbbi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
		.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
	VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cb };

	/* 6 */
	STEP(6, "clear");
	{
		VkImageCreateInfo imci = { .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D,
			.format = VK_FORMAT_R8G8B8A8_UNORM, .extent = { 64, 64, 1 }, .mipLevels = 1, .arrayLayers = 1,
			.samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
			.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
			.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED };
		VkImage img;
		VkDeviceMemory imem;
		if ((r = vkCreateImage(dev, &imci, NULL, &img)) != VK_SUCCESS) {
			FAIL("vkCreateImage %d", r);
			goto compute;
		}
		vkGetImageMemoryRequirements(dev, img, &mr);
		VkMemoryAllocateInfo imai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size,
			.memoryTypeIndex = find_memory(&mp, mr.memoryTypeBits, 0) };
		if (vkAllocateMemory(dev, &imai, NULL, &imem) != VK_SUCCESS || vkBindImageMemory(dev, img, imem, 0)) {
			FAIL("image memory");
			goto compute;
		}
		memset(words, 0, 64 * 64 * 4);
		VkImageSubresourceRange range = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
		vkBeginCommandBuffer(cb, &cbbi);
		VkImageMemoryBarrier b1 = { .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
			.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
			.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
			.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = img, .subresourceRange = range };
		vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL,
		    0, NULL, 1, &b1);
		VkClearColorValue colour = { .float32 = { 1.0f, 0.5f, 0.25f, 1.0f } };
		vkCmdClearColorImage(cb, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &colour, 1, &range);
		VkImageMemoryBarrier b2 = b1;
		b2.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
		b2.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
		b2.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
		b2.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
		vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0,
		    NULL, 1, &b2);
		VkBufferImageCopy copy = { .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
			.imageExtent = { 64, 64, 1 } };
		vkCmdCopyImageToBuffer(cb, img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buf, 1, &copy);
		VkBufferMemoryBarrier b3 = { .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
			.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, .dstAccessMask = VK_ACCESS_HOST_READ_BIT,
			.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
			.buffer = buf, .size = VK_WHOLE_SIZE };
		vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 0, NULL, 1, &b3,
		    0, NULL);
		vkEndCommandBuffer(cb);
		r = vkQueueSubmit(queue, 1, &si, VK_NULL_HANDLE);
		if (r == VK_SUCCESS)
			r = vkQueueWaitIdle(queue);
		uint32_t expect = 0xff4080ff; /* A B G R, little-endian: R=0xff G=0x80 B=0x40 A=0xff */
		unsigned wrong = 0;
		for (int p = 0; p < 64 * 64; p++)
			if (words[p] != expect)
				wrong++;
		if (r == VK_SUCCESS && !wrong)
			OK("4096 of 4096 pixels %#010x", expect);
		else if (r == VK_SUCCESS && noop) {
			unchecked++;
			printf("submitted and waited for; not checked: no GPU ran it (HAKIOS_DRM_NOOP)\n");
		} else
			FAIL("submit/wait %d; %u of 4096 pixels wrong (the first: %#010x, expected %#010x)", r, wrong,
			    words[0], expect);
	}

	/* 7 */
	STEP(7, "timeline");
	if (!timeline || !vkWaitSemaphores) {
		FAIL("no timeline semaphores here");
	} else {
		VkSemaphoreTypeCreateInfo stci = { .sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO,
			.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE, .initialValue = 0 };
		VkSemaphoreCreateInfo sci = { .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, .pNext = &stci };
		VkSemaphore sem;
		r = vkCreateSemaphore(dev, &sci, NULL, &sem);
		if (r != VK_SUCCESS) {
			FAIL("vkCreateSemaphore (timeline) %d", r);
		} else {
			uint64_t value = 7;
			VkTimelineSemaphoreSubmitInfo tssi = { .sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO,
				.signalSemaphoreValueCount = 1, .pSignalSemaphoreValues = &value };
			VkSubmitInfo ssi = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .pNext = &tssi,
				.signalSemaphoreCount = 1, .pSignalSemaphores = &sem };
			r = vkQueueSubmit(queue, 1, &ssi, VK_NULL_HANDLE);
			VkSemaphoreWaitInfo swi = { .sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO, .semaphoreCount = 1,
				.pSemaphores = &sem, .pValues = &value };
			VkResult rw = r == VK_SUCCESS ? vkWaitSemaphores(dev, &swi, 2000000000ull) : r;
			uint64_t now = 0;
			if (vkGetSemaphoreCounterValue)
				vkGetSemaphoreCounterValue(dev, sem, &now);
			if (rw == VK_SUCCESS && now == 7)
				OK("signalled to 7 by a submit, the CPU's wait returned, the counter reads %llu",
				    (unsigned long long)now);
			else
				FAIL("submit %d, wait %d, counter %llu", r, rw, (unsigned long long)now);
		}
	}

compute:
	/* 8 */
	STEP(8, "compute");
	{
		VkShaderModuleCreateInfo smci = { .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
			.codeSize = sizeof(shader), .pCode = shader };
		VkShaderModule sm;
		if ((r = vkCreateShaderModule(dev, &smci, NULL, &sm)) != VK_SUCCESS) {
			FAIL("vkCreateShaderModule %d", r);
			goto done;
		}
		VkDescriptorSetLayoutBinding bind = { 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1,
			VK_SHADER_STAGE_COMPUTE_BIT, NULL };
		VkDescriptorSetLayoutCreateInfo dslci = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
			.bindingCount = 1, .pBindings = &bind };
		VkDescriptorSetLayout dsl;
		vkCreateDescriptorSetLayout(dev, &dslci, NULL, &dsl);
		VkPipelineLayoutCreateInfo plci = { .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
			.setLayoutCount = 1, .pSetLayouts = &dsl };
		VkPipelineLayout pl;
		vkCreatePipelineLayout(dev, &plci, NULL, &pl);
		VkComputePipelineCreateInfo cpi = { .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
			.stage = { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
				.stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = sm, .pName = "main" },
			.layout = pl };
		VkPipeline pipe;
		if ((r = vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &cpi, NULL, &pipe)) != VK_SUCCESS) {
			FAIL("vkCreateComputePipelines %d (the shader did not compile for this GPU)", r);
			goto done;
		}
		VkDescriptorPoolSize ps = { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1 };
		VkDescriptorPoolCreateInfo dpci = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
			.maxSets = 1, .poolSizeCount = 1, .pPoolSizes = &ps };
		VkDescriptorPool dp;
		vkCreateDescriptorPool(dev, &dpci, NULL, &dp);
		VkDescriptorSetAllocateInfo dsai = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
			.descriptorPool = dp, .descriptorSetCount = 1, .pSetLayouts = &dsl };
		VkDescriptorSet ds;
		vkAllocateDescriptorSets(dev, &dsai, &ds);
		VkDescriptorBufferInfo dbi = { buf, 0, 4096 * 4 };
		VkWriteDescriptorSet wds = { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = ds,
			.descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &dbi };
		vkUpdateDescriptorSets(dev, 1, &wds, 0, NULL);
		memset(words, 0, 4096 * 4);
		vkResetCommandBuffer(cb, 0);
		vkBeginCommandBuffer(cb, &cbbi);
		vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipe);
		vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pl, 0, 1, &ds, 0, NULL);
		vkCmdDispatch(cb, 4096 / 64, 1, 1);
		VkMemoryBarrier mb = { .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
			.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT, .dstAccessMask = VK_ACCESS_HOST_READ_BIT };
		vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &mb, 0,
		    NULL, 0, NULL);
		vkEndCommandBuffer(cb);
		r = vkQueueSubmit(queue, 1, &si, VK_NULL_HANDLE);
		if (r == VK_SUCCESS)
			r = vkQueueWaitIdle(queue);
		unsigned wrong = 0;
		for (uint32_t i = 0; i < 4096; i++)
			if (words[i] != 2 * i + 1)
				wrong++;
		if (r == VK_SUCCESS && !wrong)
			OK("4096 of 4096 words 2i+1 (word 1234 = %u)", words[1234]);
		else if (r == VK_SUCCESS && noop) {
			unchecked++;
			printf("compiled for this GPU, dispatched and waited for; not checked: no GPU ran it (HAKIOS_DRM_NOOP)\n");
		} else
			FAIL("submit/wait %d; %u of 4096 words wrong (word 1234 = %u, expected 2469)", r, wrong,
			    words[1234]);
	}

done:
	if (unchecked)
		printf("vk_smoke: %d of %d steps passed, %d not checkable without a GPU\n", passed, total, unchecked);
	else
		printf("vk_smoke: %d of %d steps passed\n", passed, total);
	return passed + unchecked == 8 ? 0 : 1;
}
