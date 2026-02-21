#pragma once

#include <string>
#include <vector>
#include <SFML/Graphics.hpp>
#include "json.hpp"

using json = nlohmann::json;

// Load and render help content from embedded template
std::vector<std::string> loadHelpContent(const json& config);

// Draw help overlay on window
void drawHelp(
    std::shared_ptr<sf::RenderWindow> window,
    const std::vector<std::string>& helpLines,
    const sf::Font& font,
    unsigned int fontSize,
    const json& config
);
