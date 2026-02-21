#pragma once

#include <SFML/Graphics.hpp>
#include <SFML/Network.hpp>
#include <string>
#include <map>
#include <memory>
#include <filesystem>
#include <cmath>

namespace fs = std::filesystem;

class MapViewer {
public:
    MapViewer(const std::string& cacheDir, size_t maxCacheSizeMB, int windowWidth = 1024, int windowHeight = 768, bool inlineMode = false, int minZoom = 2, int maxZoom = 18);
    
    // Open map window centered on GPS coordinates
    void showMap(double latitude, double longitude, int initialZoom = 15);
    
    // Close the map window
    void close();
    
    // Update map to show new GPS coordinates (keeps window open)
    void updateGPS(double latitude, double longitude);
    
    // Update marker position only, without recentering the map
    void updateMarkerOnly(double latitude, double longitude);
    
    // Check if a GPS point is visible in the current map view
    bool isPointVisible(double lat, double lon) const;
    
    // Check if map window is open
    bool isOpen() const;
    
    // Process events and render (call in main loop)
    void update();
    
    // Handle window resize (resizes map display without re-downloading tiles)
    void onWindowResize(int newWidth, int newHeight);
    
    // Navigation request types
    // Returns: -1 for prev, 0 for none, 1 for next
    int getNavigationRequest();
    
    // Get folder navigation request: -1 for prev folder, 0 for none, 1 for next folder
    int getFolderNavigationRequest();
    
    // Check if close was requested
    bool isCloseRequested() const { return closeRequested; }
    
    // Handle a single event (for inline mode, with mouse position offset)
    void handleEvent(const sf::Event& event, sf::Vector2i mouseOffset = sf::Vector2i(0, 0));
    
    // Get the rendered texture for inline mode (returns nullptr if not in inline mode or not shown)
    const sf::Texture* getTexture() const;
    
    // Get map state for display
    double getCenterLat() const { return centerLat; }
    double getCenterLon() const { return centerLon; }
    int getCurrentZoom() const { return currentZoom; }
    bool isLoadingTiles() const { return isDownloadingTiles || (totalVisibleTiles > 0 && tilesLoaded < totalVisibleTiles); }
    
    // Set GPS points to display (as small circles on the map)
    void setGPSPoints(const std::vector<std::pair<double, double>>& points) { gpsPoints = points; }
    
    // Check if a GPS point is within the center 50% rectangle (25%-75%) of the visible map
    // Returns true if point should be kept in view without recentering
    bool isPointInStayPutArea(double lat, double lon) const;
    
    // Tile coordinate structure (public for cache-osm functionality)
    struct TileCoord {
        int x, y, zoom;
        bool operator<(const TileCoord& other) const {
            if (zoom != other.zoom) return zoom < other.zoom;
            if (x != other.x) return x < other.x;
            return y < other.y;
        }
    };
    
private:
    // Get current render target
    sf::RenderTarget* getRenderTarget();
    sf::Vector2u getRenderSize() const;
    
    // Convert GPS to tile coordinates
    TileCoord latLonToTile(double lat, double lon, int zoom);
    
    // Get tile bounds for lat/lon at zoom
    void latLonToPixel(double lat, double lon, int zoom, double& pixX, double& pixY) const;
    void pixelToLatLon(double pixX, double pixY, int zoom, double& lat, double& lon) const;
    
    // Tile management
    fs::path getTilePath(const TileCoord& coord);
    bool isTileCached(const TileCoord& coord);
    void downloadTile(const TileCoord& coord);
    std::shared_ptr<sf::Texture> loadTile(const TileCoord& coord);
    std::shared_ptr<sf::Texture> getTileFromCache(const TileCoord& coord);
    
    // Cache management
    void cleanCache();
    size_t getCacheSize();
    
    // Rendering
    void render();
    void updateTileLoadingState();
    void drawTiles();
    void drawCachedTiles();
    void drawMarker();
    void drawProgressIndicator();
    
    // Event handling
    void handleEvents();
    
    // Progress tracking
    void updateDownloadProgress();
    int getDownloadProgress() const;
    
    // Window and rendering state
    std::unique_ptr<sf::RenderWindow> window;
    std::unique_ptr<sf::RenderTexture> renderTexture;  // For inline mode
    bool inlineMode;  // If true, renders to texture instead of separate window
    sf::Font font;
    
    // Window size settings
    int windowWidth;
    int windowHeight;
    int minZoom;
    int maxZoom;
    
    // Map state
    double centerLat, centerLon;
    int currentZoom;
    double markerLat;
    double markerLon;
    std::vector<std::pair<double, double>> gpsPoints;  // Additional GPS points to display (lat, lon pairs)
    
    // View state
    sf::Vector2f viewOffset;  // Pan offset in pixels
    bool isDragging;
    sf::Vector2i lastMousePos;
    
    // Animation state
    bool isAnimating;
    double animStartLat, animStartLon;
    double animTargetLat, animTargetLon;
    float animProgress;  // 0.0 to 1.0
    static constexpr float ANIM_SPEED = 0.08f;  // Animation speed per frame
    
    // Tile cache
    std::map<TileCoord, std::shared_ptr<sf::Texture>> tileTextures;
    std::string cacheDirectory;
    size_t maxCacheSizeMB;
    
    // Download queue and progress
    std::map<TileCoord, bool> downloadedTiles;  // Track download attempts
    std::map<TileCoord, bool> loadedTiles;      // Track successfully loaded tiles
    int totalVisibleTiles;  // Tiles needed for current view
    int tilesLoaded;        // Tiles successfully loaded
    bool isLoadingNewLocation;  // Flag to show white screen while loading new location
    bool isDownloadingTiles;    // Flag set when actively downloading tiles (even during pan/drag)
    
    // Navigation request tracking
    int navigationRequest = 0;       // -1 for prev, 0 for none, 1 for next (images)
    int folderNavigationRequest = 0;  // -1 for prev, 0 for none, 1 for next (folders)
    bool closeRequested = false;       // Request to close the map window
    int framesSinceLocationChange = 0; // Frames since updateGPS was called, used to force white screen delay
    
    // Constants
    static constexpr int TILE_SIZE = 256;
    static constexpr const char* TILE_SERVER_HOST = "tile.openstreetmap.org";
    static constexpr const char* USER_AGENT = "ImageViewer/1.0";
};
