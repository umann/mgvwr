#include "help.h"
#include "html_utils.h"
#include "utils.h"
#include <filesystem>
#include <inja/inja.hpp>
#include <sstream>


static constexpr const char *HTML_INJA_TEMPLATE = R"(
<!DOCTYPE html> 
<html lang="en">    
<head>        
    <title>MgVwr Help</title>
</head>
<body>
<p>Keys</p>
<table>
    <tr><td>Esc</td><td>Exit Help / App</td></tr>
    <tr><td>F1</td><td>Toggle Help</td></tr>
    <tr><td>Right</td><td>Next image in folder</td></tr>
    <tr><td>Left</td><td>Previous image in folder</td></tr>
    <tr><td>PageDown</td><td>Next folder / Next page of thumbs within folder</td></tr>
    <tr><td>PageUp</td><td>Prev folder / Prev page of thumbs within folder</td></tr>
    <tr><td>Home</td><td>First image in folder</td></tr>
    <tr><td>End</td><td>Last image in folder</td></tr>
    <tr><td>.</td><td>Toggle Map</td></tr>
    <tr><td>Backspace</td><td>Up to Thumbnails</td></tr>
    <tr><td>0</td><td>Zoom Map to Default</td></tr>
    <tr><td>F</td><td>Toggle Fullscreen</td></tr>
    <tr><td>Enter</td><td>Open selected image/folder of thumb</td></tr>
    <tr><td>MouseWheel</td><td>Next / Prev</td></tr>
    <tr><td>Ctrl MouseWheel</td><td>Zoom Map / Change Thumb Size</td></tr>{% for filter in config.filters %}
    <tr><td>{{ filter.key }}</td><td>Toggle Filter: {{ filter.expression }}</td></tr>
{% endfor %}
</table>
</body>
</html>
)";

std::vector<std::string> loadHelpContent(const json &config) {
    std::vector<std::string> helpLines;

    try {
        // Prepare data for template - pass the entire config
        json templateData;
        templateData["config"] = config;

        // Render HTML template with inja, then convert the first table to plain text.
        inja::Environment env;
        std::string rendered = env.render(HTML_INJA_TEMPLATE, templateData);
        std::string helpText = first_html_table_to_text(rendered);

        if (helpText.empty()) {
            helpText = rendered;
        }

        // Simple line splitting for plain text
        std::istringstream stream(helpText);
        std::string line;
        while (std::getline(stream, line)) {
            helpLines.push_back(line);
        }

        return helpLines;
    } catch (const std::exception &e) {
        helpLines.clear();
        helpLines.push_back("Error loading help: " + std::string(e.what()));
        return helpLines;
    }
}

bool loadMonospaceFont(sf::Font &monoFont, const json &config) {
    // Try to load a monospace font for help display
    std::string os = getOs();

    std::vector<std::string> monoFontPaths;

    // Get font paths from config

    for (const auto &fontPath : config["font"]["by_os"][os]["monospace"]) {
        monoFontPaths.push_back(fontPath.get<std::string>());
    }

    for (const auto &path : monoFontPaths) {
        if (monoFont.openFromFile(path)) {
            return true;
        }
    }
    return false;
}

void drawHelp(std::shared_ptr<sf::RenderWindow> window, const std::vector<std::string> &helpLines,
              const sf::Font &font, unsigned int fontSize, const json &config) {
    if (helpLines.empty())
        return;

    // Try to use monospace font for better alignment
    sf::Font monoFont;
    const sf::Font &displayFont = loadMonospaceFont(monoFont, config) ? monoFont : font;

    auto windowSize = window->getSize();
    const float lineSpacing = static_cast<float>(fontSize + 6);

    // Calculate help box dimensions
    float maxWidth = 0.0f;
    for (const auto &line : helpLines) {
        sf::String sfLine = sf::String::fromUtf8(line.begin(), line.end());
        sf::Text text(displayFont, sfLine, fontSize);
        auto bounds = text.getLocalBounds();
        maxWidth = std::max(maxWidth, bounds.size.x);
    }

    // Make window bigger - increased padding and spacing
    float boxWidth = maxWidth + 120.0f;
    float boxHeight = helpLines.size() * lineSpacing + 100.0f;

    // Center the box
    float boxX = (windowSize.x - boxWidth) / 2.0f;
    float boxY = (windowSize.y - boxHeight) / 2.0f;

    // Draw semi-transparent background
    sf::RectangleShape background(sf::Vector2f(boxWidth, boxHeight));
    background.setPosition({boxX, boxY});
    background.setFillColor(sf::Color(0, 0, 0, 220));
    window->draw(background);

    // Draw border
    sf::RectangleShape border(sf::Vector2f(boxWidth, boxHeight));
    border.setPosition({boxX, boxY});
    border.setFillColor(sf::Color::Transparent);
    border.setOutlineColor(sf::Color(144, 238, 144));
    border.setOutlineThickness(2.0f);
    window->draw(border);

    // Draw help lines
    const float textX = boxX + 60.0f;
    const float textY = boxY + 40.0f;
    const float contentStartY = textY;
    for (size_t i = 0; i < helpLines.size(); i++) {
        sf::String sfLine = sf::String::fromUtf8(helpLines[i].begin(), helpLines[i].end());
        sf::Text text(displayFont, sfLine, fontSize);
        text.setFillColor(sf::Color::White);
        float yPos = contentStartY + static_cast<float>(i) * lineSpacing;
        text.setPosition({textX, yPos});
        window->draw(text);
    }
}

void usage(const std::string &programName, const std::string &message) {
    if (!message.empty()) {
        log_stderr(message);
        log_stderr("");
    }

    log_stderr("Usage: ", programName, " <image_file>");
    log_stderr("   or: ", programName, " --config <file> <image_file>");
    log_stderr("   or: ", programName, " --self-check [--config <file>]");
    log_stderr("   or: ", programName, " --cache [--config <file>] [--zoom <level>] <path> [<path> ...]");
    log_stderr("   or: ", programName, " --exiftool <image_file>");
    log_stderr("   or: ", programName, " --poor <image_file>");
    log_stderr("   or: ", programName, " [--config <file>] [--zoom <level>] <image_file>");
    log_stderr("");
    log_stderr("Supported formats: JPG, PNG, BMP, GIF, TIFF");
}
