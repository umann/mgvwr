#include "map_viewer.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>


#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static std::string getTimestamp() {
    auto now = std::chrono::system_clock::now();
    auto time = std::chrono::system_clock::to_time_t(now);
    std::stringstream ss;
    ss << std::put_time(std::localtime(&time), "%Y-%m-%d %H:%M:%S");
    return ss.str();
}

MapViewer::MapViewer(const std::string &cacheDir, size_t maxCacheSizeMB, int windowWidth, int windowHeight,
                     bool inlineMode, int minZoom, int maxZoom)
    : cacheDirectory(cacheDir), maxCacheSizeMB(maxCacheSizeMB), windowWidth(windowWidth), windowHeight(windowHeight),
      inlineMode(inlineMode), minZoom(minZoom), maxZoom(maxZoom), currentZoom(15), centerLat(0.0), centerLon(0.0),
      markerLat(0.0), markerLon(0.0), isDragging(false),
      totalVisibleTiles(1) // Start as 1 to show progress bar initially
      ,
      tilesLoaded(0), isDownloadingTiles(false), isLoadingNewLocation(false), isAnimating(false), animProgress(0.0f) {
    // Create cache directory if it doesn't exist
    fs::create_directories(cacheDirectory);
}

void MapViewer::showMap(double latitude, double longitude, int initialZoom) {
    markerLat = latitude;
    markerLon = longitude;
    centerLat = latitude;
    centerLon = longitude;
    currentZoom = std::clamp(initialZoom, minZoom, maxZoom);
    viewOffset = sf::Vector2f(0.f, 0.f);

    if (inlineMode) {
        // Create render texture for inline rendering
        if (!renderTexture) {
            renderTexture = std::make_unique<sf::RenderTexture>();
        }
        if (!renderTexture->resize(sf::Vector2u(windowWidth, windowHeight))) {
            std::cerr << "[" << getTimestamp() << "] Failed to create render texture for inline map" << std::endl;
            return;
        }
        std::cout << "[" << getTimestamp() << "] [Map Viewer] Opened inline at " << latitude << ", " << longitude
                  << " zoom " << initialZoom << " (" << windowWidth << "x" << windowHeight
                  << "), isLoadingNewLocation=1" << std::endl;
    } else {
        // Create window with configured size
        sf::VideoMode mode(sf::Vector2u(windowWidth, windowHeight));
        window = std::make_unique<sf::RenderWindow>(
            mode, "Map View - " + std::to_string(latitude) + ", " + std::to_string(longitude), sf::Style::Default);
        window->setFramerateLimit(60);
        std::cout << "[" << getTimestamp() << "] [Map Viewer] Opened at " << latitude << ", " << longitude << " zoom "
                  << initialZoom << " (" << windowWidth << "x" << windowHeight << "), isLoadingNewLocation=1"
                  << std::endl;
    }

    // Mark as loading for initial display
    isLoadingNewLocation = true;
    framesSinceLocationChange = 0;

    // Reset all request flags when opening a new window
    closeRequested = false;
    navigationRequest = 0;
    folderNavigationRequest = 0;

    std::cout << "[" << getTimestamp() << "] [Map Viewer] Opened at " << latitude << ", " << longitude << " zoom "
              << currentZoom << " (" << windowWidth << "x" << windowHeight
              << "), isLoadingNewLocation=" << isLoadingNewLocation << std::endl;
}

void MapViewer::updateGPS(double latitude, double longitude) {
    if (!isOpen()) {
        return;
    }

    markerLat = latitude;
    markerLon = longitude;

    // Check if new position is visible in current view
    bool targetVisible = isPointVisible(latitude, longitude);

    if (targetVisible && !isPointInStayPutArea(latitude, longitude)) {
        // New point is visible but outside stay-put area - animate to it
        isAnimating = true;
        animStartLat = centerLat;
        animStartLon = centerLon;
        animTargetLat = latitude;
        animTargetLon = longitude;
        animProgress = 0.0f;
        // Don't set centerLat/centerLon here - animation will update them
        return; // Skip the jumping and tile clearing below
    }

    // Otherwise jump immediately (position not visible or animation disabled)
    centerLat = latitude;
    centerLon = longitude;
    viewOffset = sf::Vector2f(0.f, 0.f);

    // Update window title if using separate window
    if (window && window->isOpen()) {
        window->setTitle("Map View - " + std::to_string(latitude) + ", " + std::to_string(longitude));
    }

    // Mark as loading new location and clear tiles
    isLoadingNewLocation = true;
    framesSinceLocationChange = 0; // Start frame counter
    tileTextures.clear();
    tilesLoaded = 0;
    totalVisibleTiles = 1; // Reset to 1 to show loading

    std::cout << "[" << getTimestamp() << "] [Map Viewer] Updated to " << latitude << ", " << longitude << std::endl;
}

void MapViewer::updateMarkerOnly(double latitude, double longitude) {
    if (!isOpen()) {
        return;
    }

    // Only update marker position, keep map centered at its current location
    markerLat = latitude;
    markerLon = longitude;
    // Do NOT change centerLat, centerLon or viewOffset - keep map centered where it is
}

bool MapViewer::isPointVisible(double lat, double lon) const {
    if (!isOpen()) {
        return false;
    }

    // Convert point to pixel coordinates at current zoom
    double pointPixX, pointPixY;
    latLonToPixel(lat, lon, currentZoom, pointPixX, pointPixY);

    // Convert center to pixel coordinates
    double centerPixX, centerPixY;
    latLonToPixel(centerLat, centerLon, currentZoom, centerPixX, centerPixY);

    // Calculate pixel distance from center
    double driftX = pointPixX - centerPixX;
    double driftY = pointPixY - centerPixY;

    // Get render size
    sf::Vector2u renderSize = inlineMode ? renderTexture->getSize() : window->getSize();

    // Check if point is within visible area (with some margin)
    double maxDriftX = renderSize.x / 2.0;
    double maxDriftY = renderSize.y / 2.0;

    return std::abs(driftX) < maxDriftX && std::abs(driftY) < maxDriftY;
}

void MapViewer::close() {
    if (window && window->isOpen()) {
        window->close();
        std::cout << "[" << getTimestamp() << "] [Map Viewer] Closed" << std::endl;
    }
    if (renderTexture) {
        renderTexture.reset();
        std::cout << "[" << getTimestamp() << "] [Map Viewer] Inline mode closed" << std::endl;
    }
}

sf::RenderTarget *MapViewer::getRenderTarget() {
    return inlineMode ? static_cast<sf::RenderTarget *>(renderTexture.get())
                      : static_cast<sf::RenderTarget *>(window.get());
}

sf::Vector2u MapViewer::getRenderSize() const {
    return inlineMode ? renderTexture->getSize() : window->getSize();
}

const sf::Texture *MapViewer::getTexture() const {
    if (inlineMode && renderTexture) {
        return &renderTexture->getTexture();
    }
    return nullptr;
}

int MapViewer::getNavigationRequest() {
    int request = navigationRequest;
    navigationRequest = 0; // Reset after reading
    return request;
}

int MapViewer::getFolderNavigationRequest() {
    int request = folderNavigationRequest;
    folderNavigationRequest = 0; // Reset after reading
    return request;
}

bool MapViewer::isPointInStayPutArea(double lat, double lon) const {
    // Calculate pixel coordinates for the point and map center
    double pointPixX, pointPixY;
    latLonToPixel(lat, lon, currentZoom, pointPixX, pointPixY);

    double centerPixX, centerPixY;
    latLonToPixel(centerLat, centerLon, currentZoom, centerPixX, centerPixY);

    // Calculate distance from center in pixels
    double driftX = pointPixX - centerPixX;
    double driftY = pointPixY - centerPixY;

    // Check if within center 50% rectangle (25%-75% in both directions)
    // This means the drift should be within ±25% of window dimensions
    double maxDriftX = windowWidth * 0.25;
    double maxDriftY = windowHeight * 0.25;

    return std::abs(driftX) <= maxDriftX && std::abs(driftY) <= maxDriftY;
}

bool MapViewer::isOpen() const {
    return (window && window->isOpen()) || (inlineMode && renderTexture);
}

void MapViewer::update() {
    if (!isOpen())
        return;

    if (!inlineMode) {
        handleEvents();
    }

    // Update animation if in progress
    if (isAnimating) {
        animProgress += ANIM_SPEED;
        if (animProgress >= 1.0f) {
            // Animation complete
            animProgress = 1.0f;
            centerLat = animTargetLat;
            centerLon = animTargetLon;
            isAnimating = false;
        } else {
            // Ease-out interpolation for smooth deceleration
            float eased = 1.0f - std::pow(1.0f - animProgress, 3.0f);
            centerLat = animStartLat + (animTargetLat - animStartLat) * eased;
            centerLon = animStartLon + (animTargetLon - animStartLon) * eased;
        }
    }

    render();
}

void MapViewer::onWindowResize(int newWidth, int newHeight) {
    windowWidth = newWidth;
    windowHeight = newHeight;

    // Resize render texture if in inline mode
    if (inlineMode && renderTexture) {
        if (!renderTexture->resize(sf::Vector2u(newWidth, newHeight))) {
            std::cerr << "[" << getTimestamp() << "] Failed to resize render texture" << std::endl;
        }
    }

    // If standalone window exists, it will be resized by the OS, but we update our size tracking
}

MapViewer::TileCoord MapViewer::latLonToTile(double lat, double lon, int zoom) {
    TileCoord coord;
    coord.zoom = zoom;

    double n = std::pow(2.0, zoom);
    coord.x = static_cast<int>((lon + 180.0) / 360.0 * n);

    double latRad = lat * M_PI / 180.0;
    coord.y = static_cast<int>((1.0 - std::asinh(std::tan(latRad)) / M_PI) / 2.0 * n);

    return coord;
}

void MapViewer::latLonToPixel(double lat, double lon, int zoom, double &pixX, double &pixY) const {
    double n = std::pow(2.0, zoom);
    pixX = (lon + 180.0) / 360.0 * n * TILE_SIZE;

    double latRad = lat * M_PI / 180.0;
    pixY = (1.0 - std::asinh(std::tan(latRad)) / M_PI) / 2.0 * n * TILE_SIZE;
}

void MapViewer::pixelToLatLon(double pixX, double pixY, int zoom, double &lat, double &lon) const {
    double n = std::pow(2.0, zoom);
    lon = pixX / (n * TILE_SIZE) * 360.0 - 180.0;

    double latRad = std::atan(std::sinh(M_PI * (1.0 - 2.0 * pixY / (n * TILE_SIZE))));
    lat = latRad * 180.0 / M_PI;
}

fs::path MapViewer::getTilePath(const TileCoord &coord) {
    std::string zoomStr = std::to_string(coord.zoom);
    std::string xStr = std::to_string(coord.x);
    std::string yStr = std::to_string(coord.y);

    return fs::path(cacheDirectory) / zoomStr / xStr / (yStr + ".png");
}

bool MapViewer::isTileCached(const TileCoord &coord) {
    return fs::exists(getTilePath(coord));
}

void MapViewer::downloadTile(const TileCoord &coord) {
    // Check if already attempted download
    if (downloadedTiles.find(coord) != downloadedTiles.end()) {
        return; // Already tried
    }
    downloadedTiles[coord] = true;

    // Check if already cached
    if (isTileCached(coord)) {
        return;
    }

    // Build HTTPS URL (OSM now requires HTTPS)
    std::string url = "https://" + std::string(TILE_SERVER_HOST) + "/" + std::to_string(coord.zoom) + "/" +
                      std::to_string(coord.x) + "/" + std::to_string(coord.y) + ".png";

    // Prepare output file path
    fs::path tilePath = getTilePath(coord);
    fs::create_directories(tilePath.parent_path());

    // Use curl for HTTPS support
    std::string command =
        "curl -s -f -L -A \"" + std::string(USER_AGENT) + "\" -o \"" + tilePath.string() + "\" \"" + url + "\"";

    int result = system(command.c_str());

    if (result == 0 && fs::exists(tilePath) && fs::file_size(tilePath) > 0) {
        std::cout << "[" << getTimestamp() << "] [OSM Tile] Downloaded: " << coord.zoom << "/" << coord.x << "/"
                  << coord.y << std::endl;
    } else {
        // Clean up failed download
        if (fs::exists(tilePath)) {
            fs::remove(tilePath);
        }
        std::cerr << "[" << getTimestamp() << "] [OSM Tile] Failed: " << coord.zoom << "/" << coord.x << "/" << coord.y
                  << std::endl;
    }
}

std::shared_ptr<sf::Texture> MapViewer::loadTile(const TileCoord &coord) {
    // Check if already loaded
    auto it = tileTextures.find(coord);
    if (it != tileTextures.end()) {
        return it->second;
    }

    // Try to download if not cached
    if (!isTileCached(coord)) {
        downloadTile(coord);
    }

    // Load from cache
    if (isTileCached(coord)) {
        auto texture = std::make_shared<sf::Texture>();
        if (texture->loadFromFile(getTilePath(coord).string())) {
            tileTextures[coord] = texture;
            return texture;
        }
    }

    return nullptr;
}

void MapViewer::cleanCache() {
    size_t currentSize = getCacheSize();
    size_t maxSize = maxCacheSizeMB * 1024 * 1024;

    if (currentSize <= maxSize) {
        return; // Under limit
    }

    // TODO: Implement LRU cache cleanup
    // For now, just log warning
    std::cout << "Cache size " << (currentSize / 1024 / 1024) << " MB exceeds limit " << maxCacheSizeMB << " MB"
              << std::endl;
}

size_t MapViewer::getCacheSize() {
    size_t totalSize = 0;

    try {
        for (const auto &entry : fs::recursive_directory_iterator(cacheDirectory)) {
            if (entry.is_regular_file()) {
                totalSize += entry.file_size();
            }
        }
    } catch (...) {
        // Ignore errors
    }

    return totalSize;
}

void MapViewer::handleEvent(const sf::Event &event, sf::Vector2i mouseOffset) {
    // For inline mode: handle a single event with mouse position translation
    if (const auto *keyEvent = event.getIf<sf::Event::KeyPressed>()) {
        std::cout << "[" << getTimestamp() << "] [Map Viewer] Key pressed:" << static_cast<int>(keyEvent->code)
                  << std::endl;
        switch (keyEvent->code) {
        case sf::Keyboard::Key::Escape:
            if (window)
                window->close();
            break;
        case sf::Keyboard::Key::Period: // . key to toggle map
            closeRequested = true;
            break;
        case sf::Keyboard::Key::Left:
            navigationRequest = -1; // Request previous image
            break;
        case sf::Keyboard::Key::Right:
            navigationRequest = 1; // Request next image
            break;
        case sf::Keyboard::Key::PageDown:
            folderNavigationRequest = 1; // Request next folder
            std::cout << "[" << getTimestamp() << "] [Map Viewer] PageDown - next folder requested" << std::endl;
            break;
        case sf::Keyboard::Key::PageUp:
            folderNavigationRequest = -1; // Request previous folder
            std::cout << "[" << getTimestamp() << "] [Map Viewer] PageUp - prev folder requested" << std::endl;
            break;
        case sf::Keyboard::Key::End:
            std::cout << "[" << getTimestamp() << "] [Map Viewer] End key pressed" << std::endl;
            // End key - reset to marker position
            centerLat = markerLat;
            centerLon = markerLon;
            viewOffset = sf::Vector2f(0.f, 0.f);
            break;
        case sf::Keyboard::Key::Equal: // + key
        case sf::Keyboard::Key::Add:
            if (currentZoom < maxZoom) {
                currentZoom++;
                tileTextures.clear(); // Clear loaded tiles for new zoom
            }
            break;
        case sf::Keyboard::Key::Hyphen: // - key
        case sf::Keyboard::Key::Subtract:
            if (currentZoom > minZoom) {
                currentZoom--;
                tileTextures.clear(); // Clear loaded tiles for new zoom
            }
            break;
        case sf::Keyboard::Key::Home:
            std::cout << "[" << getTimestamp() << "] [Map Viewer] Home key pressed" << std::endl;
            // Reset to marker position
            centerLat = markerLat;
            centerLon = markerLon;
            viewOffset = sf::Vector2f(0.f, 0.f);
            break;
        default:
            break;
        }
    } else if (const auto *mouseButtonEvent = event.getIf<sf::Event::MouseButtonPressed>()) {
        if (mouseButtonEvent->button == sf::Mouse::Button::Left) {
            isDragging = true;
            lastMousePos = sf::Vector2i(mouseButtonEvent->position.x - mouseOffset.x,
                                        mouseButtonEvent->position.y - mouseOffset.y);
        } else if (mouseButtonEvent->button == sf::Mouse::Button::Middle) {
            // Middle mouse button to toggle map viewing
            closeRequested = true;
        }
    } else if (const auto *mouseButtonEvent = event.getIf<sf::Event::MouseButtonReleased>()) {
        if (mouseButtonEvent->button == sf::Mouse::Button::Left) {
            isDragging = false;
        }
    } else if (const auto *mouseMoveEvent = event.getIf<sf::Event::MouseMoved>()) {
        if (isDragging) {
            sf::Vector2i currentPos(mouseMoveEvent->position.x - mouseOffset.x,
                                    mouseMoveEvent->position.y - mouseOffset.y);
            sf::Vector2i delta = currentPos - lastMousePos;
            viewOffset.x += delta.x;
            viewOffset.y += delta.y;

            // Update center lat/lon based on pan
            double centerPixX, centerPixY;
            latLonToPixel(centerLat, centerLon, currentZoom, centerPixX, centerPixY);
            centerPixX -= viewOffset.x;
            centerPixY -= viewOffset.y;
            pixelToLatLon(centerPixX, centerPixY, currentZoom, centerLat, centerLon);

            viewOffset = sf::Vector2f(0.f, 0.f);
            lastMousePos = currentPos;
        }
    } else if (const auto *wheelEvent = event.getIf<sf::Event::MouseWheelScrolled>()) {
        // Check modifier keys for different actions
        bool ctrlHeld = sf::Keyboard::isKeyPressed(sf::Keyboard::Key::LControl) ||
                        sf::Keyboard::isKeyPressed(sf::Keyboard::Key::RControl);
        bool shiftHeld = sf::Keyboard::isKeyPressed(sf::Keyboard::Key::LShift) ||
                         sf::Keyboard::isKeyPressed(sf::Keyboard::Key::RShift);

        if (ctrlHeld) {
            // Ctrl+Wheel: Zoom in/out (scroll up = zoom in, scroll down = zoom out)
            if (wheelEvent->delta > 0 && currentZoom < maxZoom) {
                currentZoom++;
                std::cout << "[" << getTimestamp() << "] [Map Viewer] Zoom level: " << currentZoom << std::endl;
                tileTextures.clear();
                tilesLoaded = 0; // Reset progress bar
            } else if (wheelEvent->delta < 0 && currentZoom > minZoom) {
                currentZoom--;
                std::cout << "[" << getTimestamp() << "] [Map Viewer] Zoom level: " << currentZoom << std::endl;
                tileTextures.clear();
                tilesLoaded = 0; // Reset progress bar
            }
        } else if (shiftHeld) {
            // Shift+Wheel: Folder navigation (up = prev folder, down = next folder)
            if (wheelEvent->delta > 0) {
                folderNavigationRequest = -1;
            } else if (wheelEvent->delta < 0) {
                folderNavigationRequest = 1;
            }
        } else {
            // Plain Wheel: Image navigation (up = prev, down = next)
            if (wheelEvent->delta > 0) {
                navigationRequest = -1;
            } else if (wheelEvent->delta < 0) {
                navigationRequest = 1;
            }
        }
    }
}

void MapViewer::handleEvents() {
    if (!window || !window->isOpen())
        return;

    while (const auto event = window->pollEvent()) {
        if (event->is<sf::Event::Closed>()) {
            window->close();
        } else {
            // Handle the event using the shared handler
            handleEvent(*event);
        }
    }
}

void MapViewer::render() {
    // Increment frame counter if loading
    if (isLoadingNewLocation) {
        framesSinceLocationChange++;
        std::cout << "[" << getTimestamp() << "] [Map Viewer] Render frame: " << framesSinceLocationChange
                  << ", isLoading=" << isLoadingNewLocation << std::endl;
    }

    // Always update tile loading state (even while loading a new location)
    // But skip the very first frame to ensure white screen is shown
    if (framesSinceLocationChange > 1) {
        updateTileLoadingState();
    }

    // Get render target (window or renderTexture)
    sf::RenderTarget *target = inlineMode ? static_cast<sf::RenderTarget *>(renderTexture.get())
                                          : static_cast<sf::RenderTarget *>(window.get());

    if (!target)
        return;

    // Choose clear color based on loading state
    target->clear(isLoadingNewLocation ? sf::Color::White : sf::Color(200, 200, 200));

    // Draw tiles and marker only when fully loaded
    if (!isLoadingNewLocation) {
        drawCachedTiles();
        drawMarker();
    }

    drawProgressIndicator();

    if (inlineMode) {
        renderTexture->display();
    } else {
        window->display();
    }
}

void MapViewer::updateTileLoadingState() {
    auto windowSize = getRenderSize();
    double centerPixX, centerPixY;
    latLonToPixel(centerLat, centerLon, currentZoom, centerPixX, centerPixY);

    // Apply view offset
    centerPixX -= viewOffset.x;
    centerPixY -= viewOffset.y;

    // Calculate visible tile range
    int screenCenterX = windowSize.x / 2;
    int screenCenterY = windowSize.y / 2;

    int minTileX = static_cast<int>((centerPixX - screenCenterX) / TILE_SIZE) - 1;
    int maxTileX = static_cast<int>((centerPixX + screenCenterX) / TILE_SIZE) + 1;
    int minTileY = static_cast<int>((centerPixY - screenCenterY) / TILE_SIZE) - 1;
    int maxTileY = static_cast<int>((centerPixY + screenCenterY) / TILE_SIZE) + 1;

    // Clamp to valid tile range
    int maxTileIndex = (1 << currentZoom) - 1;
    minTileX = std::max(0, minTileX);
    maxTileX = std::min(maxTileIndex, maxTileX);
    minTileY = std::max(0, minTileY);
    maxTileY = std::min(maxTileIndex, maxTileY);

    // Count total visible tiles
    int tilesX = maxTileX - minTileX + 1;
    int tilesY = maxTileY - minTileY + 1;
    int newTotal = tilesX * tilesY;

    if (newTotal != totalVisibleTiles) {
        totalVisibleTiles = newTotal;
        std::cout << "[" << getTimestamp() << "] [Map Viewer] Viewport: " << tilesX << "x" << tilesY << " = "
                  << totalVisibleTiles << " tiles" << std::endl;
    }

    tilesLoaded = 0;

    if (isLoadingNewLocation) {
        std::cout << "[" << getTimestamp() << "] [Map Viewer] updateTileLoadingState: loading tiles..." << std::endl;
    }

    // Check if any tiles will need downloading before we start the process
    bool anyTilesToDownload = false;
    for (int tileX = minTileX; tileX <= maxTileX; tileX++) {
        for (int tileY = minTileY; tileY <= maxTileY; tileY++) {
            TileCoord coord{tileX, tileY, currentZoom};
            // Check if tile needs downloading (not cached, not in memory, not attempted yet)
            if (tileTextures.find(coord) == tileTextures.end() && !isTileCached(coord) &&
                downloadedTiles.find(coord) == downloadedTiles.end()) {
                anyTilesToDownload = true;
                break;
            }
        }
        if (anyTilesToDownload)
            break;
    }

    // Set flag BEFORE starting downloads - this ensures the flag is true during the frame when downloads happen
    if (anyTilesToDownload) {
        isDownloadingTiles = true;
    }

    // Load tiles (synchronous downloads happen here, blocking the thread)
    for (int tileX = minTileX; tileX <= maxTileX; tileX++) {
        for (int tileY = minTileY; tileY <= maxTileY; tileY++) {
            TileCoord coord{tileX, tileY, currentZoom};
            auto texture = loadTile(coord);
            if (texture) {
                tilesLoaded++; // Count loaded tiles
            }
        }
    }

    // Keep the downloading flag set for at least one more frame after downloads complete
    // Clear it only on the next call if all tiles are loaded
    if (tilesLoaded >= totalVisibleTiles && !anyTilesToDownload) {
        // Both: all tiles loaded AND no downloads were attempted this frame
        // This ensures the flag persists for one frame even after synchronous downloads
        isDownloadingTiles = false;
    }

    if (isLoadingNewLocation) {
        std::cout << "[" << getTimestamp() << "] [Map Viewer] Tiles loaded: " << tilesLoaded << "/"
                  << totalVisibleTiles << ", framesSinceLocationChange=" << framesSinceLocationChange << std::endl;
    }

    // If we've loaded all tiles for the current view, mark loading as complete
    // (require framesSinceLocationChange >= 2 to ensure at least 1-2 frames of white screen)
    if (isLoadingNewLocation && tilesLoaded >= totalVisibleTiles && totalVisibleTiles > 0 &&
        framesSinceLocationChange >= 2) {
        isLoadingNewLocation = false;
        std::cout << "[" << getTimestamp() << "] [Map Viewer] Map tiles loaded, " << tilesLoaded << "/"
                  << totalVisibleTiles << std::endl;
    }
}

void MapViewer::drawCachedTiles() {
    auto windowSize = getRenderSize();
    double centerPixX, centerPixY;
    latLonToPixel(centerLat, centerLon, currentZoom, centerPixX, centerPixY);

    // Apply view offset
    centerPixX -= viewOffset.x;
    centerPixY -= viewOffset.y;

    // Calculate visible tile range
    int screenCenterX = windowSize.x / 2;
    int screenCenterY = windowSize.y / 2;

    int minTileX = static_cast<int>((centerPixX - screenCenterX) / TILE_SIZE) - 1;
    int maxTileX = static_cast<int>((centerPixX + screenCenterX) / TILE_SIZE) + 1;
    int minTileY = static_cast<int>((centerPixY - screenCenterY) / TILE_SIZE) - 1;
    int maxTileY = static_cast<int>((centerPixY + screenCenterY) / TILE_SIZE) + 1;

    // Clamp to valid tile range
    int maxTileIndex = (1 << currentZoom) - 1;
    minTileX = std::max(0, minTileX);
    maxTileX = std::min(maxTileIndex, maxTileX);
    minTileY = std::max(0, minTileY);
    maxTileY = std::min(maxTileIndex, maxTileY);

    // Draw cached tiles
    for (int tileX = minTileX; tileX <= maxTileX; tileX++) {
        for (int tileY = minTileY; tileY <= maxTileY; tileY++) {
            TileCoord coord{tileX, tileY, currentZoom};
            auto texture = getTileFromCache(coord);

            if (texture) {
                sf::Sprite sprite(*texture);

                // Calculate screen position
                float screenX = screenCenterX + (tileX * TILE_SIZE - centerPixX) + viewOffset.x;
                float screenY = screenCenterY + (tileY * TILE_SIZE - centerPixY) + viewOffset.y;

                sprite.setPosition(sf::Vector2f(screenX, screenY));
                getRenderTarget()->draw(sprite);
            }
        }
    }
}

// Get tile from cache without trying to download
std::shared_ptr<sf::Texture> MapViewer::getTileFromCache(const TileCoord &coord) {
    auto it = tileTextures.find(coord);
    if (it != tileTextures.end()) {
        return it->second;
    }
    return nullptr;
}

void MapViewer::drawTiles() {
    auto windowSize = window->getSize();
    double centerPixX, centerPixY;
    latLonToPixel(centerLat, centerLon, currentZoom, centerPixX, centerPixY);

    // Apply view offset
    centerPixX -= viewOffset.x;
    centerPixY -= viewOffset.y;

    // Calculate visible tile range
    int screenCenterX = windowSize.x / 2;
    int screenCenterY = windowSize.y / 2;

    int minTileX = static_cast<int>((centerPixX - screenCenterX) / TILE_SIZE) - 1;
    int maxTileX = static_cast<int>((centerPixX + screenCenterX) / TILE_SIZE) + 1;
    int minTileY = static_cast<int>((centerPixY - screenCenterY) / TILE_SIZE) - 1;
    int maxTileY = static_cast<int>((centerPixY + screenCenterY) / TILE_SIZE) + 1;

    // Clamp to valid tile range
    int maxTileIndex = (1 << currentZoom) - 1;
    minTileX = std::max(0, minTileX);
    maxTileX = std::min(maxTileIndex, maxTileX);
    minTileY = std::max(0, minTileY);
    maxTileY = std::min(maxTileIndex, maxTileY);

    // Count total visible tiles - only log when it changes
    int tilesX = maxTileX - minTileX + 1;
    int tilesY = maxTileY - minTileY + 1;
    int newTotal = tilesX * tilesY;

    if (newTotal != totalVisibleTiles) {
        totalVisibleTiles = newTotal;
        std::cout << "[" << getTimestamp() << "] [Map Viewer] Viewport: " << tilesX << "x" << tilesY << " = "
                  << totalVisibleTiles << " tiles" << std::endl;
    }

    tilesLoaded = 0;

    // Draw tiles
    for (int tileX = minTileX; tileX <= maxTileX; tileX++) {
        for (int tileY = minTileY; tileY <= maxTileY; tileY++) {
            TileCoord coord{tileX, tileY, currentZoom};
            auto texture = loadTile(coord);

            if (texture) {
                sf::Sprite sprite(*texture);
                tilesLoaded++; // Count loaded tiles

                // Calculate screen position
                float screenX = screenCenterX + (tileX * TILE_SIZE - centerPixX) + viewOffset.x;
                float screenY = screenCenterY + (tileY * TILE_SIZE - centerPixY) + viewOffset.y;

                sprite.setPosition(sf::Vector2f(screenX, screenY));
                getRenderTarget()->draw(sprite);
            }
        }
    }

    // If we've loaded all tiles for the current view, mark loading as complete
    if (isLoadingNewLocation && tilesLoaded >= totalVisibleTiles && totalVisibleTiles > 0 &&
        framesSinceLocationChange >= 2) {
        isLoadingNewLocation = false;
        std::cout << "[" << getTimestamp() << "] [Map Viewer] Map tiles loaded" << std::endl;
    }
}

void MapViewer::drawMarker() {
    auto windowSize = getRenderSize();

    // Calculate marker screen position
    double markerPixX, markerPixY;
    latLonToPixel(markerLat, markerLon, currentZoom, markerPixX, markerPixY);

    double centerPixX, centerPixY;
    latLonToPixel(centerLat, centerLon, currentZoom, centerPixX, centerPixY);
    centerPixX -= viewOffset.x;
    centerPixY -= viewOffset.y;

    int screenCenterX = windowSize.x / 2;
    int screenCenterY = windowSize.y / 2;

    float markerScreenX = screenCenterX + (markerPixX - centerPixX) + viewOffset.x;
    float markerScreenY = screenCenterY + (markerPixY - centerPixY) + viewOffset.y;

    // Draw marker pin (simple circle)
    sf::CircleShape marker(8.f);
    marker.setFillColor(sf::Color::Red);
    marker.setOutlineColor(sf::Color::White);
    marker.setOutlineThickness(2.f);
    marker.setOrigin(sf::Vector2f(8.f, 8.f));
    marker.setPosition(sf::Vector2f(markerScreenX, markerScreenY));
    getRenderTarget()->draw(marker);

    // Draw all GPS points as half-size circles
    for (const auto &[pointLat, pointLon] : gpsPoints) {
        double pointPixX, pointPixY;
        latLonToPixel(pointLat, pointLon, currentZoom, pointPixX, pointPixY);

        float pointScreenX = screenCenterX + (pointPixX - centerPixX) + viewOffset.x;
        float pointScreenY = screenCenterY + (pointPixY - centerPixY) + viewOffset.y;

        // Check if point is visible in the map area
        if (pointScreenX >= -10 && pointScreenX <= windowSize.x + 10 && pointScreenY >= -10 &&
            pointScreenY <= windowSize.y + 10) {

            sf::CircleShape gpsMarker(4.f); // Half size: 4.f instead of 8.f
            gpsMarker.setFillColor(sf::Color::Red);
            gpsMarker.setOrigin(sf::Vector2f(4.f, 4.f));
            gpsMarker.setPosition(sf::Vector2f(pointScreenX, pointScreenY));
            getRenderTarget()->draw(gpsMarker);
        }
    }

    // Draw zoom level indicator
    // Note: Text rendering requires font - skipped for now
}
int MapViewer::getDownloadProgress() const {
    if (totalVisibleTiles == 0 || tilesLoaded >= totalVisibleTiles)
        return 100;
    return (tilesLoaded * 100) / totalVisibleTiles;
}

void MapViewer::drawProgressIndicator() {
    // Progress indicator is not meaningful with synchronous tile downloads
    // The isDownloadingTiles flag is used instead to show "Loading map..." text
    // Disable the visual progress bar since it doesn't accurately reflect download progress
    return;
}
