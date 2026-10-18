// pspFileSystem for PPSSPP's savedata code, on host files: PSP paths are mapped with the runtime's
// io_host_path() (ms0:/ is g_save_root, case-insensitive lookup), so saves land in real PSP-style
// directories (ms0:/PSP/SAVEDATA/<GAMEID><NAME>/PARAM.SFO, ICON0.PNG, data files...).
// Implements the subset of PPSSPP's MetaFileSystem API that Core/Dialog and PPGe use; semantics follow
// PPSSPP's DirectoryFileSystem (listings skip "." and "..", errors are negative PSP error codes).
// Unlike a PSP, MkDir also creates missing parent directories.
//
// Part of the rcsm utility library. GPL-2.0+ (see LICENSE.TXT).

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <map>
#include <string>
#include <vector>

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include "Common/CommonTypes.h"
#include "Common/Log.h"
#include "Core/FileSystems/MetaFileSystem.h"
#include "../audio/stubs/kernel_bridge.h"

UtilityFileSystem pspFileSystem;

static const int ERROR_FILE_NOT_FOUND = (int)0x80010002;
static const int ERROR_FILE_EXISTS = (int)0x80010011;
static const int ERROR_NO_SPACE = (int)0x8001001C;
static const int ERROR_BAD_FD = (int)0x80020323;

namespace {
struct OpenHostFile {
	int fd;
	std::string path;
};
std::map<u32, OpenHostFile> openFiles;
u32 nextHandle = 1;

bool HostPath(std::string_view psp, std::string &out) {
	std::string p(psp);
	char buf[1024];
	if (io_host_path(p.c_str(), buf, sizeof(buf)) < 0)
		return false;
	out = buf;
	// Directory paths are passed with a trailing '/'; the host doesn't care, but keep names clean.
	while (out.size() > 1 && out.back() == '/')
		out.pop_back();
	return true;
}

void FillTimes(PSPFileInfo &info, const struct stat &st) {
	time_t a = st.st_atime, c = st.st_ctime, m = st.st_mtime;
	localtime_r(&a, &info.atime);
	localtime_r(&c, &info.ctime);
	localtime_r(&m, &info.mtime);
}

PSPFileInfo InfoFromStat(const std::string &name, const struct stat &st) {
	PSPFileInfo info;
	info.name = name;
	info.exists = true;
	info.type = S_ISDIR(st.st_mode) ? FILETYPE_DIRECTORY : FILETYPE_NORMAL;
	info.size = S_ISDIR(st.st_mode) ? 0 : (s64)st.st_size;
	info.access = st.st_mode & 0777;
	FillTimes(info, st);
	return info;
}

void MkDirs(const std::string &path) {
	std::string cur;
	for (size_t i = 0; i <= path.size(); i++) {
		if (i == path.size() || (path[i] == '/' && i > 0)) {
			cur = path.substr(0, i);
			mkdir(cur.c_str(), 0755);
		}
	}
}
}  // namespace

std::vector<PSPFileInfo> UtilityFileSystem::GetDirListing(std::string_view path, bool *exists) {
	std::vector<PSPFileInfo> result;
	std::string host;
	DIR *dir = HostPath(path, host) ? opendir(host.c_str()) : nullptr;
	if (exists)
		*exists = dir != nullptr;
	if (!dir)
		return result;
	while (struct dirent *e = readdir(dir)) {
		if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
			continue;
		struct stat st;
		if (stat((host + "/" + e->d_name).c_str(), &st) != 0)
			continue;
		result.push_back(InfoFromStat(e->d_name, st));
	}
	closedir(dir);
	return result;
}

int UtilityFileSystem::OpenFile(std::string filename, FileAccess access, const char *devicename) {
	std::string host;
	if (!HostPath(filename, host))
		return ERROR_FILE_NOT_FOUND;
	int flags = 0;
	const bool rd = access & FILEACCESS_READ, wr = access & (FILEACCESS_WRITE | FILEACCESS_APPEND);
	flags = rd && wr ? O_RDWR : wr ? O_WRONLY : O_RDONLY;
	if (access & FILEACCESS_APPEND) flags |= O_APPEND;
	if (access & FILEACCESS_CREATE) flags |= O_CREAT;
	if (access & FILEACCESS_TRUNCATE) flags |= O_TRUNC;
	if (access & FILEACCESS_EXCL) flags |= O_EXCL;
	int fd = open(host.c_str(), flags | O_CLOEXEC, 0644);
	if (fd < 0) {
		if (!(access & FILEACCESS_PPSSPP_QUIET))
			DEBUG_LOG(Log::IO, "utility fs: can't open %s (%s): %s", filename.c_str(), host.c_str(), strerror(errno));
		return errno == EEXIST ? ERROR_FILE_EXISTS : errno == ENOSPC ? ERROR_NO_SPACE : ERROR_FILE_NOT_FOUND;
	}
	u32 handle = nextHandle++;
	openFiles[handle] = OpenHostFile{fd, host};
	return (int)handle;
}

void UtilityFileSystem::CloseFile(u32 handle) {
	auto it = openFiles.find(handle);
	if (it == openFiles.end())
		return;
	close(it->second.fd);
	openFiles.erase(it);
}

size_t UtilityFileSystem::ReadFile(u32 handle, u8 *pointer, s64 size) {
	auto it = openFiles.find(handle);
	if (it == openFiles.end() || size <= 0)
		return 0;
	size_t done = 0;
	while (done < (size_t)size) {
		ssize_t n = read(it->second.fd, pointer + done, (size_t)size - done);
		if (n <= 0)
			break;
		done += n;
	}
	return done;
}

size_t UtilityFileSystem::WriteFile(u32 handle, const u8 *pointer, s64 size) {
	auto it = openFiles.find(handle);
	if (it == openFiles.end() || size <= 0)
		return 0;
	size_t done = 0;
	while (done < (size_t)size) {
		ssize_t n = write(it->second.fd, pointer + done, (size_t)size - done);
		if (n <= 0)
			break;
		done += n;
	}
	return done;
}

size_t UtilityFileSystem::SeekFile(u32 handle, s32 position, FileMove type) {
	auto it = openFiles.find(handle);
	if (it == openFiles.end())
		return (size_t)(s32)ERROR_BAD_FD;
	int whence = type == FILEMOVE_BEGIN ? SEEK_SET : type == FILEMOVE_END ? SEEK_END : SEEK_CUR;
	off_t r = lseek(it->second.fd, position, whence);
	return r < 0 ? 0 : (size_t)r;
}

PSPFileInfo UtilityFileSystem::GetFileInfo(std::string filename) {
	PSPFileInfo info;
	std::string host;
	size_t slash = filename.find_last_of("/:", filename.size() > 1 ? filename.size() - 2 : std::string::npos);
	info.name = slash == std::string::npos ? filename : filename.substr(slash + 1);
	while (!info.name.empty() && info.name.back() == '/')
		info.name.pop_back();
	struct stat st;
	if (!HostPath(filename, host) || stat(host.c_str(), &st) != 0)
		return info;
	return InfoFromStat(info.name, st);
}

bool UtilityFileSystem::MkDir(const std::string &dirname) {
	std::string host;
	if (!HostPath(dirname, host))
		return false;
	MkDirs(host);
	struct stat st;
	return stat(host.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

bool UtilityFileSystem::RmDir(const std::string &dirname) {
	// PPSSPP's DirectoryFileSystem::RmDir removes the directory with its contents (savedata delete).
	std::string host;
	if (!HostPath(dirname, host))
		return false;
	DIR *dir = opendir(host.c_str());
	if (!dir)
		return false;
	std::vector<std::string> entries;
	while (struct dirent *e = readdir(dir)) {
		if (strcmp(e->d_name, ".") && strcmp(e->d_name, ".."))
			entries.push_back(e->d_name);
	}
	closedir(dir);
	for (const std::string &e : entries) {
		std::string sub = dirname;
		if (!sub.empty() && sub.back() != '/')
			sub += '/';
		sub += e;
		struct stat st;
		if (stat((host + "/" + e).c_str(), &st) == 0 && S_ISDIR(st.st_mode))
			RmDir(sub);
		else
			unlink((host + "/" + e).c_str());
	}
	return rmdir(host.c_str()) == 0;
}

bool UtilityFileSystem::RemoveFile(const std::string &filename) {
	std::string host;
	return HostPath(filename, host) && unlink(host.c_str()) == 0;
}

int UtilityFileSystem::ReadEntireFile(const std::string &filename, std::vector<u8> &data, bool quiet) {
	int handle = OpenFile(filename, (FileAccess)(FILEACCESS_READ | (quiet ? FILEACCESS_PPSSPP_QUIET : 0)));
	if (handle < 0)
		return handle;
	struct stat st;
	fstat(openFiles[handle].fd, &st);
	data.resize((size_t)st.st_size);
	size_t n = ReadFile(handle, data.data(), (s64)data.size());
	CloseFile(handle);
	data.resize(n);
	return 0;
}
