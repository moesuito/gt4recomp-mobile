// SPDX-License-Identifier: GPL-3.0+
//
// TASK-186 round 7: the Auto API rule, tested against ARMSX2's own decision function. The six strings are the ones the
// brief names; the expected answers come from ARMSX2's table (Adreno, Mali Valhall v9/v11 and MT6897 -> Vulkan; every
// other GPU stays on OpenGL ES). Run on the device with adb:
//
//   adb push gt4gs_auto_test /data/local/tmp/ && adb shell /data/local/tmp/gt4gs_auto_test
//
// Exit code 0 means every case matched.

#include "pcsx2/GS/GSUtil.h"

#include <cstdio>

namespace
{
struct Case
{
	const char* name;
	const char* vendor;
	const char* renderer;
	bool vulkan;
};
} // namespace

int main()
{
	const Case cases[] = {
		{"Adreno 740", "Qualcomm", "Adreno (TM) 740", true},
		{"Mali-G78", "ARM", "Mali-G78", true},
		{"Mali-G57", "ARM", "Mali-G57", true},
		{"PowerVR", "Imagination Technologies", "PowerVR Rogue GE8320", false},
		{"Xclipse", "Samsung", "Xclipse 940", false},
		{"llvmpipe", "Mesa", "llvmpipe (LLVM 21.1.2, 256 bits)", false},
	};

	int failures = 0;
	for (const Case& c : cases)
	{
		const bool got = GSUtil::AndroidAutoPrefersVulkan(c.vendor, c.renderer, "OpenGL ES 3.2");
		std::printf("%-12s %-42s -> %-9s (expected %-9s) %s\n", c.name, c.renderer,
			got ? "Vulkan" : "OpenGL ES", c.vulkan ? "Vulkan" : "OpenGL ES", GSUtil::AndroidAutoRendererReason());
		if (got != c.vulkan)
			failures++;
	}
	std::printf("%d case(s) failed\n", failures);
	return failures == 0 ? 0 : 1;
}
