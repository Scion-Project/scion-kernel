/*
 * The microSD card as an external disk in IOKit (A, 2026-11-21; docs/devices/x1slte/lifs-loader.md).
 *
 * userfsd (Files' external drives, with Apple's lifs.kext loaded by bsd/dev/redwood_kextload.c) matches an IOMedia under
 * an IOBlockStorageDevice whose "Protocol Characteristics" say External (its launchd plist), then reads "BSD Name" and
 * opens /dev/<name>. This kernel has no IOStorage family, so these are minimal classes with those names, carrying the
 * properties userfsd reads; the reads themselves are bsd/dev/redwood_sdcard.c's character devices.
 *
 *   IOResources
 *    +- IOBlockStorageDevice ("SD Card", External, removable)
 *        +- IOMedia diskN    (whole card, Whole, FDisk_partition_scheme)
 *            +- IOMedia diskNs1  (the first MBR partition, Leaf)
 */

#include <IOKit/IOService.h>
#include <IOKit/IOLib.h>
#include <libkern/c++/OSDictionary.h>
#include <libkern/c++/OSString.h>
#include <libkern/c++/OSNumber.h>
#include <libkern/c++/OSBoolean.h>

class IOBlockStorageDevice : public IOService
{
	OSDeclareDefaultStructors(IOBlockStorageDevice);
};
OSDefineMetaClassAndStructors(IOBlockStorageDevice, IOService);

class IOMedia : public IOService
{
	OSDeclareDefaultStructors(IOMedia);
};
OSDefineMetaClassAndStructors(IOMedia, IOService);

static void
set_num(IOService *s, const char *key, uint64_t v, unsigned bits)
{
	OSNumber *n = OSNumber::withNumber(v, bits);

	if (n != NULL) {
		s->setProperty(key, n);
		n->release();
	}
}

static IOMedia *
media(IOService *parent, const char *bsd, unsigned unit, int major, int minor, uint64_t base, uint64_t bytes, bool whole,
    const char *content, bool writable)
{
	IOMedia *m = new IOMedia;

	if (m == NULL || !m->init()) {
		return NULL;
	}
	m->setProperty("BSD Name", bsd);
	set_num(m, "BSD Unit", unit, 32);
	set_num(m, "BSD Major", (uint64_t)major, 32);
	set_num(m, "BSD Minor", (uint64_t)minor, 32);
	set_num(m, "Size", bytes, 64);
	set_num(m, "Base", base, 64);
	set_num(m, "Preferred Block Size", 512, 64);
	m->setProperty("Whole", whole ? kOSBooleanTrue : kOSBooleanFalse);
	m->setProperty("Leaf", whole ? kOSBooleanFalse : kOSBooleanTrue);
	m->setProperty("Writable", writable ? kOSBooleanTrue : kOSBooleanFalse);
	m->setProperty("Removable", kOSBooleanTrue);
	m->setProperty("Ejectable", kOSBooleanTrue);
	m->setProperty("Open", kOSBooleanFalse);
	m->setProperty("Content", content);
	m->setProperty("Content Hint", content);
	if (!whole) {
		set_num(m, "Partition ID", 1, 32);
	}
	if (!m->attach(parent)) {
		m->release();
		return NULL;
	}
	return m;
}

extern "C" {
/* 0 when published; called once, from the SD driver's probe thread */
int redwood_sdmedia_publish(unsigned unit, int major, uint64_t whole_blocks, uint64_t part_start, uint64_t part_blocks,
    unsigned part_type, int writable);
int
redwood_sdmedia_publish(unsigned unit, int major, uint64_t whole_blocks, uint64_t part_start, uint64_t part_blocks,
    unsigned part_type, int writable)
{
	IOBlockStorageDevice *dev = new IOBlockStorageDevice;
	OSDictionary *pc = OSDictionary::withCapacity(2), *dc = OSDictionary::withCapacity(3);
	IOMedia *whole, *part;
	char name[16], pname[16];
	const char *ptype;

	if (dev == NULL || pc == NULL || dc == NULL || !dev->init()) {
		return -1;
	}
	pc->setObject("Physical Interconnect", OSString::withCString("SATA"));
	pc->setObject("Physical Interconnect Location", OSString::withCString("External"));
	dev->setProperty("Protocol Characteristics", pc);
	/* userfsd searches the IOService parent chain for this key directly. */
	dev->setProperty("Physical Interconnect Location", "External");
	dc->setObject("Product Name", OSString::withCString("SD Card"));
	dc->setObject("Vendor Name", OSString::withCString("microSD"));
	dc->setObject("Medium Type", OSString::withCString("Solid State"));
	dev->setProperty("Device Characteristics", dc);
	dev->setProperty("removable", kOSBooleanTrue);
	pc->release();
	dc->release();
	if (!dev->attach(IOService::getResourceService())) {
		dev->release();
		return -1;
	}
	dev->registerService();

	/* MBR types: 0x07 exFAT/NTFS, 0x0b/0x0c FAT32, 0x0e/0x06 FAT16 */
	ptype = part_type == 0x07 ? "Windows_NTFS" : (part_type == 0x0b || part_type == 0x0c) ? "DOS_FAT_32" :
	    (part_type == 0x06 || part_type == 0x0e) ? "DOS_FAT_16" : "Windows_NTFS";
	snprintf(name, sizeof(name), "disk%u", unit);
	snprintf(pname, sizeof(pname), "disk%us1", unit);
	whole = media(dev, name, unit, major, 0, 0, whole_blocks * 512, true, "FDisk_partition_scheme", false);
	if (whole == NULL) {
		return -1;
	}
	whole->registerService();
	part = media(whole, pname, unit, major, 1, part_start * 512, part_blocks * 512, false, ptype, writable != 0);
	if (part == NULL) {
		return -1;
	}
	part->registerService();
	IOLog("sdcard: IOKit: %s (%llu MB) and %s (%s, %llu MB) under an external IOBlockStorageDevice\n", name,
	    whole_blocks / 2048, pname, ptype, part_blocks / 2048);
	return 0;
}
}
