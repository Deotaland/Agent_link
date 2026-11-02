#pragma once
struct DIR {};
struct dirent { char d_name[256]; };
DIR* opendir(const char*);
dirent* readdir(DIR*);
int closedir(DIR*);
