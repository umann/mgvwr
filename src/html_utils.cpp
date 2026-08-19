#include "html_utils.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <iomanip>
#include <limits>
#include <sstream>
#include <vector>

namespace {

std::string trimCopy(std::string value) {
    auto isNotSpace = [](unsigned char ch) { return !std::isspace(ch); };
    value.erase(value.begin(), std::find_if(value.begin(), value.end(), isNotSpace));
    value.erase(std::find_if(value.rbegin(), value.rend(), isNotSpace).base(), value.end());
    return value;
}

std::string decodeHtmlEntities(std::string value) {
    struct Entity {
        const char *from;
        const char *to;
    };

    static constexpr Entity entities[] = {
        {"&amp;", "&"}, {"&lt;", "<"}, {"&gt;", ">"}, {"&quot;", "\""}, {"&apos;", "'"}};

    for (const auto &entity : entities) {
        std::string::size_type pos = 0;
        while ((pos = value.find(entity.from, pos)) != std::string::npos) {
            value.replace(pos, std::strlen(entity.from), entity.to);
            pos += std::strlen(entity.to);
        }
    }

    return value;
}

std::string stripHtmlTags(const std::string &value) {
    std::string output;
    output.reserve(value.size());

    bool insideTag = false;
    for (char ch : value) {
        if (ch == '<') {
            insideTag = true;
            continue;
        }
        if (ch == '>') {
            insideTag = false;
            continue;
        }
        if (!insideTag) {
            output.push_back(ch);
        }
    }

    return trimCopy(decodeHtmlEntities(output));
}

} // namespace

std::string first_html_table_to_text(const std::string &htmlStr) {
    const std::string tableOpen = "<table";
    const std::string tableClose = "</table>";

    const std::size_t tableStart = htmlStr.find(tableOpen);
    if (tableStart == std::string::npos) {
        return {};
    }

    const std::size_t tableTagEnd = htmlStr.find('>', tableStart);
    if (tableTagEnd == std::string::npos) {
        return {};
    }

    const std::size_t tableEnd = htmlStr.find(tableClose, tableTagEnd);
    if (tableEnd == std::string::npos || tableEnd <= tableTagEnd) {
        return {};
    }

    const std::string tableHtml = htmlStr.substr(tableTagEnd + 1, tableEnd - tableTagEnd - 1);

    std::vector<std::vector<std::string>> rows;
    std::size_t rowPos = 0;
    while (true) {
        const std::size_t rowStart = tableHtml.find("<tr", rowPos);
        if (rowStart == std::string::npos) {
            break;
        }

        const std::size_t rowTagEnd = tableHtml.find('>', rowStart);
        if (rowTagEnd == std::string::npos) {
            break;
        }

        const std::size_t rowEnd = tableHtml.find("</tr>", rowTagEnd);
        if (rowEnd == std::string::npos) {
            break;
        }

        const std::string rowHtml = tableHtml.substr(rowTagEnd + 1, rowEnd - rowTagEnd - 1);
        std::vector<std::string> cells;
        std::size_t cellPos = 0;
        while (true) {
            std::size_t cellStart = rowHtml.find("<td", cellPos);
            std::string closingTag = "</td>";
            if (cellStart == std::string::npos) {
                cellStart = rowHtml.find("<th", cellPos);
                closingTag = "</th>";
            }
            if (cellStart == std::string::npos) {
                break;
            }

            const std::size_t cellTagEnd = rowHtml.find('>', cellStart);
            if (cellTagEnd == std::string::npos) {
                break;
            }

            const std::size_t closeStart = rowHtml.find(closingTag, cellTagEnd);
            if (closeStart == std::string::npos) {
                break;
            }

            std::string cellHtml = rowHtml.substr(cellTagEnd + 1, closeStart - cellTagEnd - 1);
            cells.push_back(stripHtmlTags(cellHtml));
            cellPos = closeStart + closingTag.size();
        }

        if (!cells.empty()) {
            rows.push_back(std::move(cells));
        }

        rowPos = rowEnd + 5;
    }

    std::size_t columnCount = 0;
    for (const auto &row : rows) {
        columnCount = std::max(columnCount, row.size());
    }

    if (columnCount == 0) {
        return {};
    }

    std::vector<std::size_t> widths(columnCount, 0);
    for (const auto &row : rows) {
        for (std::size_t col = 0; col < row.size(); ++col) {
            widths[col] = std::max(widths[col], row[col].size());
        }
    }

    std::ostringstream out;
    for (const auto &row : rows) {
        for (std::size_t col = 0; col < row.size(); ++col) {
            if (col > 0) {
                out << "   ";
            }
            out << std::left << std::setw(static_cast<int>(widths[col])) << row[col];
        }
        out << '\n';
    }

    return out.str();
}