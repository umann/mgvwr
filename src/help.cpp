#include "help.h"
#include "utils.h"
#include <cctype>
#include <filesystem>
#include <inja/inja.hpp>
#include <sstream>

static constexpr const char *INJA_TEMPLATE = R"(Esc               Close Help / Exit App  
F1                Toggle Help  
Right             Next image in folder  
Left              Prev image in folder  
PageDown          Next folder  
PageUp            Prev folder  
Home              First image in folder  
End               Last image in folder  
.                 Toggle Map  
Enter             Toggle Fullscreen  
MouseWheel        Next/Prev  
Ctrl+MouseWheel   Zoom Map  
{% for filter in config.filters %}{{ filter.key }}                 Toggle Filter: {{ filter.expression }}  
{% endfor %})";

static constexpr const char *UNUSED_HTML_INJA_TEMPLATE_BUT_DO_NOT_DELETE_IT = R"(
<!DOCTYPE html> 
<html lang="en">    
<head>        
    <title>MgVwr Help</title>
</head>
<body>
<p>Keys</p>
<table>
    <tr><td>Right</td><td>Next image in folder</td></tr>
    <tr><td>Left</td><td>Previous image in folder</td></tr>
    <tr><td>PageDown</td><td>Next folder</td></tr>
    <tr><td>PageUp</td><td>Previous folder</td></tr>
    <tr><td>Home</td><td>Go to first image in folder</td></tr>
    <tr><td>End</td><td>Go to last image in folder</td></tr>
    <tr><td>.</td><td>Toggle Map</td></tr>
    <tr><td>Enter</td><td>Toggle Fullscreen</td></tr>
    <tr><td>F1</td><td>Toggle Help</td></tr>
    <tr><td>Esc</td><td>Close Help / App</td></tr>
    <tr><td>MouseWheel</td><td>Next/Prev</td></tr>
    <tr><td>Ctrl+MouseWheel</td><td>Zoom Map</td></tr>{% for filter in config.filters %}
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

        // Render template with inja from string
        inja::Environment env;
        std::string rendered = env.render(INJA_TEMPLATE, templateData);

        // Simple line splitting for plain text
        std::istringstream stream(rendered);
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
