// Image file reading / writing without an external library: PNG (8-bit gray / RGB / RGBA / paletted, non-interlaced), BMP (24 / 32 bit); writing: PNG (uncompressed deflate).
// For replaceable textures (textures folder next to the program).
#pragma once
#include <cstdint>
#include <string>
#include <vector>

bool loadImageFile(const std::string& path, int& w, int& h, std::vector<uint8_t>& rgba, bool* hasAlpha = nullptr);
bool savePng(const std::string& path, int w, int h, const std::vector<uint8_t>& rgba);
// bilinear resize (RGBA)
std::vector<uint8_t> resizeRgba(const std::vector<uint8_t>& src, int w, int h, int nw, int nh);
