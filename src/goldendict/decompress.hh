#pragma once
#include <vector>
#include <string>
std::vector<char> zlibDecompress(const char* bufptr, unsigned length, unsigned long adler32_checksum);
std::string decompressZlib(const char* bufptr, unsigned length);
