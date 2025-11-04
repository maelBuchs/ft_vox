#pragma once

#include <array>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <map>
#include <nlohmann/json.hpp>
#include <ostream>
#include <string>
#include <vector>

#define BLOCK_DATA_PATH "../../assets/blocks.json"

struct BlockData {
    std::string name = "no_name";
    // A map from face name ("top", "side", etc.) to texture path
    std::map<std::string, std::string> texturePaths;
    bool isDisplayable = true;
    bool isSolid = true;
    bool isTransparent = false;
    bool isFluid = false;
    bool isFlammable = false;
    float lightEmission = 0.0F;
};

static void printBlockDataVector(const std::vector<BlockData>& vec) {
    for (size_t i = 0; i < vec.size(); i++) {
        std::cout << "Block ID: " << i << ", Name: " << vec[i].name << "\n";
    }
}

class BlockRegistry {

  public:
    BlockRegistry() {
        std::ifstream f(BLOCK_DATA_PATH);
        nlohmann::json data = nlohmann::json::parse(f);

        // DYNAMIC SIZING: Find the highest block ID to auto-size the vector
        int maxId = 0;
        for (const auto& block : data) {
            int id = block["id"];
            if (id > maxId) {
                maxId = id;
            }
        }

        // Resize vector to accommodate all blocks (ID 0 to maxId)
        _blocks_data.resize(maxId + 1);

        // Load block data from JSON
        for (const auto& block : data) {
            int id = block["id"];

            _blocks_data[id].name = block["name"];

            if (block.contains("textures")) {
                const auto& textures = block["textures"];
                if (textures.contains("all")) {
                    std::string path = textures["all"];
                    _blocks_data[id].texturePaths["all"] = path;
                } else {
                    if (textures.contains("top"))
                        _blocks_data[id].texturePaths["top"] = textures["top"];
                    if (textures.contains("bottom"))
                        _blocks_data[id].texturePaths["bottom"] = textures["bottom"];
                    if (textures.contains("side"))
                        _blocks_data[id].texturePaths["side"] = textures["side"];
                }
            }

            if (block.contains("tags")) {
                const auto& tags = block["tags"];
                if (tags.contains("displayable") && !tags["displayable"].get<bool>()) {
                    _blocks_data[id].isDisplayable = false;
                }
                if (tags.contains("solid") && !tags["solid"].get<bool>()) {
                    _blocks_data[id].isSolid = false;
                }
                if (tags.contains("flammable") && tags["flammable"].get<bool>()) {
                    _blocks_data[id].isFlammable = true;
                }
                if (tags.contains("transparent") && tags["transparent"].get<bool>()) {
                    _blocks_data[id].isTransparent = true;
                }
                if (tags.contains("fluid") && tags["fluid"].get<bool>()) {
                    _blocks_data[id].isFluid = true;
                }
            }
        }

        std::cout << "[BlockRegistry] Loaded " << _blocks_data.size() << " block types\n";
        printBlockDataVector(_blocks_data);
        std::fflush(stdout);
    };
    ~BlockRegistry() = default;
    BlockRegistry(const BlockRegistry&) = delete;
    BlockRegistry& operator=(const BlockRegistry&) = delete;
    BlockRegistry(BlockRegistry&&) = delete;
    BlockRegistry& operator=(BlockRegistry&&) = delete;

    // Get the total number of blocks registered
    [[nodiscard]] size_t getBlockCount() const { return _blocks_data.size(); }

    std::string getName(int id) const { return _blocks_data[id].name; }
    bool isDisplayable(int id) const { return _blocks_data[id].isDisplayable; }
    bool isSolid(int id) const { return _blocks_data[id].isSolid; }
    bool isTransparent(int id) const { return _blocks_data[id].isTransparent; }
    bool isFluid(int id) const { return _blocks_data[id].isFluid; }
    bool isFlammable(int id) const { return _blocks_data[id].isFlammable; }

    [[nodiscard]] std::string getTexturePath(int blockId, const std::string& face) const {
        if (blockId < 0 || blockId >= static_cast<int>(_blocks_data.size()))
            return "";
        const auto& block = _blocks_data[blockId];

        // If a specific face texture is defined, use it
        if (auto it = block.texturePaths.find(face); it != block.texturePaths.end()) {
            return it->second;
        }
        // Otherwise, fall back to "side" if it exists (for North, South, East, West)
        if (face != "top" && face != "bottom") {
            if (auto it = block.texturePaths.find("side"); it != block.texturePaths.end()) {
                return it->second;
            }
        }
        // Finally, fall back to "all"
        if (auto it = block.texturePaths.find("all"); it != block.texturePaths.end()) {
            return it->second;
        }
        return "";
    }

  private:
    std::vector<BlockData> _blocks_data; // Dynamically sized based on JSON
};
