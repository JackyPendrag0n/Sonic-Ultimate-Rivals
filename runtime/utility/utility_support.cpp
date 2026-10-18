// Support code for PPSSPP's utility dialogs and PPGe in the rcsm runtime: the pieces of PPSSPP's
// core/frontend they call (config, kernel memory, controller latch, memory stick, i18n, VFS for the
// embedded PPGe atlas, OSD, system properties) implemented on the rcsm kernel, plus a few small
// helpers copied from PPSSPP (Common/StringUtils.cpp, Core/HLE/sceIo.cpp, Core/HLE/sceUtility.cpp,
// Core/Compatibility.cpp) where compiling the whole file would drag in unrelated subsystems.
//
// Part of the rcsm utility library. Copyright (c) 2012- PPSSPP Project for the copied parts.
// GPL-2.0+ (see LICENSE.TXT).

#include <algorithm>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <sys/statvfs.h>
#include <zlib.h>

#include "Common/CommonTypes.h"
#include "Common/Log.h"
#include "Common/StringUtils.h"
#include "Common/File/Path.h"
#include "Common/File/FileUtil.h"
#include "Common/File/VFS/VFS.h"
#include "Common/Data/Text/I18n.h"
#include "Common/Serialize/Serializer.h"
#include "Common/Serialize/SerializeFuncs.h"
#include "Common/System/OSD.h"
#include "Common/System/Request.h"
#include "Common/System/System.h"
#include "Core/Compatibility.h"
#include "Core/Config.h"
#include "Core/CoreParameter.h"
#include "Core/HDRemaster.h"
#include "Core/System.h"
#include "Core/HLE/sceCtrl.h"
#include "Core/HLE/sceKernelMemory.h"
#include "Core/HLE/sceKernelThread.h"
#include "Core/HLE/sceRtc.h"
#include "Core/HLE/sceUtility.h"
#include "Core/HW/Display.h"
#include "Core/HW/MemoryStick.h"
#include "GPU/GPUCommon.h"
#include "zstd.h"

#include "../audio/stubs/kernel_bridge.h"  // last: it defines short macros

extern "C" u32 fe_buttons;  // runtime/frontend.c: PSP button bits of the host controller

// ---------------------------------------------------------------- configuration
UtilityConfig g_Config;
bool g_RemasterMode = false;

CoreParameter &PSP_CoreParameter() {
	static CoreParameter param;
	return param;
}

std::string GetGameID() {
	return "";
}

// From PPSSPP Core/Compatibility.cpp: no compat flags (no compat.ini is loaded). Weak: libpspgpu.a
// has the same definition.
__attribute__((weak)) void Compatibility::Clear() {
	memset(&flags_, 0, sizeof(flags_));
	memset(&vrCompat_, 0, sizeof(vrCompat_));
}

// GetPSPLanguage() is in utility_hle.cpp. The OSK only uses this mapping for iLanguage == -1 ("auto"),
// which the fixed configuration never sets; PPSSPP loads it from langregion.ini.
const std::map<std::string, std::pair<std::string, int>, std::less<>> &GetLangValuesMapping() {
	static const std::map<std::string, std::pair<std::string, int>, std::less<>> mapping = {
		{"en_US", {"English", PSP_SYSTEMPARAM_LANGUAGE_ENGLISH}},
	};
	return mapping;
}

bool System_GetPropertyBool(SystemProperty prop) {
	// Not headless (so PPGe loads its atlas), no host keyboard for the OSK.
	return false;
}

RequestManager g_requestManager;
bool RequestManager::MakeSystemRequest(SystemRequestType type, RequesterToken token, RequestCallback callback,
	RequestFailedCallback failedCallback, std::string_view param1, std::string_view param2, int64_t param3, int64_t param4) {
	return false;  // no host UI requests (e.g. a host text input box for the OSK)
}

OnScreenDisplay g_OSD;
void OnScreenDisplay::Show(OSDType type, std::string_view text, std::string_view text2, std::string_view icon, float duration_s, const char *id) {
	if (type == OSDType::MESSAGE_ERROR || type == OSDType::MESSAGE_WARNING)
		WARN_LOG(Log::sceUtility, "%.*s %.*s", (int)text.size(), text.data(), (int)text2.size(), text2.data());
}
void OnScreenDisplay::SetFlags(std::string_view id, OSDMessageFlags flags) {}

// ---------------------------------------------------------------- kernel memory for PPGe
// One block of kernel scratch memory (rcsm kalloc, taken on first use), managed first-fit with
// 16-byte granularity. Allocation from the top is honoured like PPSSPP's BlockAllocator. PPGe needs
// ~130 KB for its display list and vertices plus 40-130 KB per savedata icon texture; it evicts
// icons when an allocation fails.
namespace {
const u32 POOL_SIZE = 0x100000;
struct PoolBlock { u32 start, size; bool used; };
struct Pool {
	std::vector<PoolBlock> blocks;
	void Init() {
		if (!blocks.empty())
			return;
		u32 base = kalloc(POOL_SIZE);
		blocks.push_back({base, POOL_SIZE, false});
	}
};
Pool pool;
}

UtilityMemoryPool kernelMemory;
UtilityMemoryPool userMemory;

u32 UtilityMemoryPool::Alloc(u32 &size, bool fromTop, const char *tag) {
	pool.Init();
	u32 need = (std::max(size, 1u) + 15) & ~15u;
	auto &blocks = pool.blocks;
	int pick = -1;
	for (int i = 0; i < (int)blocks.size(); i++) {
		if (!blocks[i].used && blocks[i].size >= need) {
			pick = i;
			if (!fromTop)
				break;
		}
	}
	if (pick < 0) {
		DEBUG_LOG(Log::sceUtility, "utility pool: out of memory for %u bytes (%s)", need, tag ? tag : "");
		return (u32)-1;
	}
	PoolBlock b = blocks[pick];
	u32 addr;
	if (b.size == need) {
		blocks[pick].used = true;
		addr = b.start;
	} else if (fromTop) {
		blocks[pick].size -= need;
		addr = b.start + b.size - need;
		blocks.insert(blocks.begin() + pick + 1, PoolBlock{addr, need, true});
	} else {
		blocks[pick] = PoolBlock{b.start, need, true};
		blocks.insert(blocks.begin() + pick + 1, PoolBlock{b.start + need, b.size - need, false});
		addr = b.start;
	}
	size = need;
	return addr;
}

bool UtilityMemoryPool::Free(u32 position) {
	auto &blocks = pool.blocks;
	for (size_t i = 0; i < blocks.size(); i++) {
		if (blocks[i].start != position || !blocks[i].used)
			continue;
		blocks[i].used = false;
		if (i + 1 < blocks.size() && !blocks[i + 1].used) {
			blocks[i].size += blocks[i + 1].size;
			blocks.erase(blocks.begin() + i + 1);
		}
		if (i > 0 && !blocks[i - 1].used) {
			blocks[i - 1].size += blocks[i].size;
			blocks.erase(blocks.begin() + i);
		}
		return true;
	}
	return false;
}

u32 UtilityMemoryPool::GetLargestFreeBlockSize() const {
	u32 best = 0;
	for (const PoolBlock &b : pool.blocks)
		if (!b.used)
			best = std::max(best, b.size);
	return pool.blocks.empty() ? POOL_SIZE : best;
}

// Volatile memory (0x08400000-0x08800000): the dialogs "lock" it while they run and, on a PSP, the
// utility overwrites it. The rcsm kernel doesn't arbitrate it and the game may be using it, so the
// lock always succeeds and the unlock reports failure: PSPDialog::FinishVolatile then never clears it.
int KernelVolatileMemLock(int type, u32 paddr, u32 psize) {
	return 0;
}
int KernelVolatileMemUnlock(int type) {
	return SCE_KERNEL_ERROR_SEMA_OVF;  // any error; nothing was really locked
}

int KernelCurThreadPriority() {
	return g_cur ? (int)g_cur->prio : 0x20;
}

int __DisplayGetNumVblanks() {
	return (int)g_vblank_count;
}

// ---------------------------------------------------------------- controller
// Like PPSSPP's sceCtrl latch: the buttons pressed since the previous read.
static u32 latchPrevButtons;
u32 __CtrlPeekButtons() {
	return fe_buttons;
}
u32 __CtrlReadLatch() {
	u32 cur = fe_buttons;
	u32 make = cur & ~latchPrevButtons;
	latchPrevButtons = cur;
	return make;
}

// ---------------------------------------------------------------- memory stick (the save directory)
MemStickState MemoryStick_State() {
	return PSP_MEMORYSTICK_STATE_INSERTED;
}
u64 MemoryStick_SectorSize() {
	return 32 * 1024;  // PSP memory sticks use 32 KB clusters
}
u64 MemoryStick_FreeSpace(std::string gameID) {
	// The host's free space, capped at the configured stick size (games mishandle huge values).
	const u64 cap = (u64)g_Config.iMemStickSizeGB << 30;
	struct statvfs vfs;
	if (g_save_root[0] && statvfs(g_save_root, &vfs) == 0) {
		u64 avail = (u64)vfs.f_bavail * vfs.f_frsize;
		return std::min(avail, cap);
	}
	return cap;
}
void MemoryStick_NotifyGameName(std::string gameID) {}

// ---------------------------------------------------------------- i18n (English)
namespace {
// PPSSPP's English text (assets/lang/en_US.ini, [Dialog] and [Error]) for the keys the dialogs use
// where it differs from the key or the code's default.
const std::map<std::string_view, std::string_view> englishText = {
	{"Corrupted Data", "Corrupted data"},
	{"Delete completed", "Delete completed."},
	{"DeleteConfirm", "This save data will be deleted. Are you sure you want to continue?"},
	{"Deleting", "Deleting\nPlease wait..."},
	{"Load completed", "Load completed."},
	{"LoadingFailed", "Unable to load data."},
	{"MsgErrorSavedataMSFull", "Memory Stick full. Check your storage space."},
	{"MsgErrorSavedataNoMS", "Memory Stick not inserted."},
	{"Save completed", "Save completed."},
	{"Saving", "Saving\nPlease wait..."},
	{"There is no data", "There is no data."},
	{"Unable to write savedata, disk may be full", "Unable to write savedata, disk may be full."},
	{"When you save, it will not work on outdated PSP Firmware anymore", "When you save, it will not work on outdated PSP firmware anymore"},
};
}
std::string_view I18NCategory::T(std::string_view key, std::string_view def) {
	auto it = englishText.find(key);
	if (it != englishText.end())
		return it->second;
	return def.empty() ? key : def;
}
const char *I18NCategory::T_cstr(const char *key, const char *def) {
	auto it = englishText.find(key);
	if (it != englishText.end())
		return it->second.data();
	return def ? def : key;
}
std::shared_ptr<I18NCategory> GetI18NCategory(I18NCat cat) {
	static std::shared_ptr<I18NCategory> category = std::make_shared<I18NCategory>();
	return category;
}

// ---------------------------------------------------------------- VFS: the embedded PPGe atlas
#include "data/ppge_atlas.inc"

UtilityVFS g_VFS;
uint8_t *UtilityVFS::ReadFile(const char *filename, size_t *size) {
	std::string_view name(filename);
	if (name == "ppge_atlas.zim") {
		uint8_t *data = new uint8_t[sizeof(g_ppgeAtlasZim)];
		memcpy(data, g_ppgeAtlasZim, sizeof(g_ppgeAtlasZim));
		*size = sizeof(g_ppgeAtlasZim);
		return data;
	}
	if (name == "ppge_atlas.meta") {
		uint8_t *data = new uint8_t[g_ppgeAtlasMetaSize];
		uLongf outSize = g_ppgeAtlasMetaSize;
		if (uncompress(data, &outSize, g_ppgeAtlasMetaZ, sizeof(g_ppgeAtlasMetaZ)) != Z_OK || outSize != g_ppgeAtlasMetaSize) {
			ERROR_LOG(Log::sceGe, "utility: can't inflate the embedded ppge_atlas.meta");
			delete[] data;
			return nullptr;
		}
		*size = g_ppgeAtlasMetaSize;
		return data;
	}
	DEBUG_LOG(Log::IO, "utility VFS: no asset %s", filename);
	*size = 0;
	return nullptr;
}

size_t ZSTD_decompress(void *dst, size_t dstCapacity, const void *src, size_t compressedSize) {
	return (size_t)-1;  // the embedded atlas isn't zstd-compressed
}

// ---------------------------------------------------------------- host files (PNG writing, unused)
namespace File {
__attribute__((weak)) FILE *OpenCFile(const Path &filename, const char *mode) {
	return nullptr;
}
__attribute__((weak)) bool Delete(const Path &filename, bool quiet) {
	return false;
}
}
__attribute__((weak)) std::string Path::GetFilename() const {
	size_t pos = path_.rfind('/');
	return pos != std::string::npos ? path_.substr(pos + 1) : path_;
}

// ---------------------------------------------------------------- savestates (not supported)
void Do(PointerWrap &p, std::string &x) { p.SetError(PointerWrap::ERROR_FAILURE); }
void Do(PointerWrap &p, std::u16string &x) { p.SetError(PointerWrap::ERROR_FAILURE); }
void Do(PointerWrap &p, tm &t) { p.SetError(PointerWrap::ERROR_FAILURE); }

// ---------------------------------------------------------------- from PPSSPP Core/HLE/sceIo.cpp
void ConvertTmToPspDateTime(ScePspDateTime &date_out, const tm &date_in, int microSeconds) {
	date_out.year = date_in.tm_year + 1900;
	date_out.month = date_in.tm_mon + 1;
	date_out.day = date_in.tm_mday;
	date_out.hour = date_in.tm_hour;
	date_out.minute = date_in.tm_min;
	date_out.second = date_in.tm_sec;
	date_out.microsecond = microSeconds;
}

// ---------------------------------------------------------------- from PPSSPP Common/StringUtils.cpp
// Weak: libpspgpu.a compiles the whole of StringUtils.cpp.
__attribute__((weak)) size_t truncate_cpy(char *dest, size_t destSize, const char *src) {
	size_t len = strlen(src);
	if (len >= destSize - 1) {
		memcpy(dest, src, destSize - 1);
		len = destSize - 1;
	} else {
		memcpy(dest, src, len);
	}
	dest[len] = '\0';
	return len;
}

__attribute__((weak)) size_t truncate_cpy(char *dest, size_t destSize, std::string_view src) {
	if (src.size() > destSize - 1) {
		memcpy(dest, src.data(), destSize - 1);
		dest[destSize - 1] = 0;
		return destSize - 1;
	} else {
		memcpy(dest, src.data(), src.size());
		dest[src.size()] = 0;
		return src.size();
	}
}

__attribute__((weak)) std::string_view StripSpaces(std::string_view str) {
	const size_t s = str.find_first_not_of(" \t\r\n");
	if (std::string::npos != s)
		return str.substr(s, str.find_last_not_of(" \t\r\n") - s + 1);
	else
		return "";
}

__attribute__((weak)) void SplitString(std::string_view str, const char delim, std::vector<std::string_view> &output) {
	size_t next = 0;
	for (size_t pos = 0, len = str.length(); pos < len; ++pos) {
		if (str[pos] == delim) {
			output.emplace_back(str.substr(next, pos - next));
			// Skip the delimiter itself.
			next = pos + 1;
		}
	}

	if (next == 0) {
		output.push_back(str);
	} else if (next < str.length()) {
		output.emplace_back(str.substr(next));
	}
}

__attribute__((weak)) std::string ReplaceAll(std::string_view input, std::string_view src, std::string_view dest) {
	size_t pos = 0;

	std::string result(input);
	if (src == dest)
		return result;

	while (true) {
		pos = result.find(src, pos);
		if (pos == std::string_view::npos)
			break;
		result.replace(pos, src.size(), dest);
		pos += dest.size();
	}
	return result;
}

// ---------------------------------------------------------------- GPU hooks used by PPGe
UtilityGpuHooks utilityGpuHooksInstance;
UtilityGpuHooks *gpu = &utilityGpuHooksInstance;
UtilityGpuStats gpuStats;
