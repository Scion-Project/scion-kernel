/*
 * The Note9's (SM-N960): the S9's list, but its rkp_region is 0xAF600000 + 36 MB (the S9's: 0xAF800000 + 32 MB), which the
 * S9's list would partly take (UEFI's map is the S9's: it can call those 2 MB at each end free); so the stretch from
 * 0xAF000000 to 0xB1FFFFFF is left out whole, with a margin. On by default: the window of a 6 GB phone is the high bank's 4 GB
 * only, and these ranges are 1.1 GB more of the low bank. (Run on an SM-N960N, November 2026.)
 */
static const uint64_t exynos9810_extra_ram_crown[][2] = {
	{ 0x80200000, 0x8F000000 }, { 0x98000000, 0xAF000000 }, { 0xB2000000, 0xB8000000 }, { 0xB8200000, 0xC0000000 },
	{ 0xC0080000, 0xCC000000 }, { 0xE3700000, 0xF0000000 }, { 0, 0 },
};

static const struct platform platform_crownlte = {
	.name = "crownlte", .io_base = 0x10000000, .io_size = 0x10000000,
	.gic_dist = 0x10101000, .gic_cpu = 0x10102000, .uart = 0,
	.cores = 8, .core_mpidr = (const uint32_t[]){ 0x000, 0x001, 0x002, 0x003, 0x100, 0x101, 0x102, 0x103 },
	.small_cores = 0x0f, .timer_hz = 26000000, .fb_base = 0xCC000000,
	.ram_log = 0xFED14000, .ram_log_size = 0x4000, .ram_dump = 0xFED10000, .ram_dump_size = 0x4000,
	.log_other_cpu = 1, .recovery_pmu = 0x14060000, .pmu_swreset = 0x400, .data_part = "USERDATA",
	.exynos_wdt = 0x10060000, .exynos_wdt_other = 0x10050000, .exynos_wdt_pmu_bit = 23,
	.parked_cores = 0x2, .affinity_unreliable = 1,
	.payload_home = 0xD0000000,
	.keep_out = { { 0xF0000000, 0x0B200000, "CP: modem_if, cp_ram_logging" }, { 0xFB200000, 0x02800000, "ABOX: abox_rmem" } },
	.extra_ram = exynos9810_extra_ram_crown,   /* (on by default; extra_ram=0 turns it off) */
	.sibling_of = &platform_starlte,
};
