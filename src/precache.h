#pragma once

#include "map_viewer.h"
#include "thumbnail_cache.h"

#include <filesystem>
#include <string>
#include <vector>

namespace fs = std::filesystem;

extern std::string g_exiftoolPath;

std::vector<MapViewer::TileCoord> calculateTilesForView(double latitude, double longitude, int zoom, int windowWidth,
                                                        int windowHeight);

int runCacheMode(const std::vector<std::string> &paths, const std::string &configPath, bool useExistingThumb,
                 CacheRefreshTarget forceRefreshTarget, bool ignoreDirMtime);
