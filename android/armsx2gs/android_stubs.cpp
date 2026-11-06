// SPDX-License-Identifier: GPL-3.0+
//
// GT4Recomp private Android test build (PCSX2-tainted, GPL), not distributed.
//
// The Android JNI services ARMSX2's app provides from Kotlin (files through Java, pad rumble, UI sounds) are not used
// by a GS-only library: nothing here opens a document, plays a sound or drives a motor. The definitions exist because
// the core's CDVD and SPU2 code references them at link time.

#include "common/HostSys.h"
#include "common/FileSystem.h"

#include "pcsx2/Input/AndroidNativeRumble.h"

#include <string>
#include <vector>

std::vector<std::string> FileSystem::FindContentChdSiblings(const char *filename) { return {}; }

int FileSystem::OpenFDFileContent(const char *filename) { return -1; }

bool FileSystem::CreateDirectoryViaJava(const char *path) { return false; }

bool FileSystem::CreateFileViaJava(const char *path) { return false; }

bool Common::PlaySoundAsync(const char *path) { return false; }

namespace Native {
void onPadRumble(int pad, int largeMotor, int smallMotor) {}
}  // namespace Native
