#pragma once
struct stat { long long st_size; long long st_mtime; };
int stat(const char*, struct stat*);
