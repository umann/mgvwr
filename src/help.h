#pragma once

#include <SFML/Graphics.hpp>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

using json = nlohmann::json;

// Load and render help content from embedded template
std::vector<std::string> loadHelpContent(const json &config);

// Draw help overlay on window
void drawHelp(std::shared_ptr<sf::RenderWindow> window, const std::vector<std::string> &helpLines,
              const sf::Font &font, unsigned int fontSize, const json &config);

// Print CLI usage to stderr. If message is not empty, prints it first, followed by an empty line.
void usage(const std::string &programName, const std::string &message = "");
