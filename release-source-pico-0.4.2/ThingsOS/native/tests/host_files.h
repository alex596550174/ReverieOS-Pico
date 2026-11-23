// SPDX-License-Identifier: Apache-2.0
#pragma once

#ifdef _WIN32
#include <direct.h>
static inline int host_mkdir(const char* path) { return _mkdir(path); }
static inline int host_rmdir(const char* path) { return _rmdir(path); }
#else
#include <sys/stat.h>
#include <unistd.h>
static inline int host_mkdir(const char* path) { return mkdir(path, 0775); }
static inline int host_rmdir(const char* path) { return rmdir(path); }
#endif
