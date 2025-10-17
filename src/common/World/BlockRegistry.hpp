#include <array>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <nlohmann/json.hpp>
#include <ostream>
#include <vector>

#define BLOCK_DATA_PATH "../../assets/blocks.json"
#define MAX_BLOCKS 10

struct BlockData {
    std::string name = "no_name";
    bool isDisplayable = true;
    bool isSolid = true;
    bool isTransparent = false;
    bool isFluid = false;
    bool isFlammable = false;
    float lightEmission = 0.0F;
};

static void printBlockDataVector(const std::vector<BlockData>& vec) {
    auto block = vec.begin();
    for (int i = 0; i < MAX_BLOCKS; i++) {
        std::cout << "Block ID: " << i << ", Name: " << block->name << "\n";
        block++;
    }
}

class BlockRegistry {

  public:
    BlockRegistry() {
        std::ifstream f(BLOCK_DATA_PATH);
        nlohmann::json data = nlohmann::json::parse(f);
        for (const auto& block : data) {
            int id = block["id"];

            if (id < MAX_BLOCKS) {
                _blocks_data[id].name = block["name"];
            }

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
        printBlockDataVector(_blocks_data);
        std::fflush(stdout);
    };
    ~BlockRegistry() = default;
    BlockRegistry(const BlockRegistry&) = delete;
    BlockRegistry& operator=(const BlockRegistry&) = delete;
    BlockRegistry(BlockRegistry&&) = delete;
    BlockRegistry& operator=(BlockRegistry&&) = delete;

    std::string getName(int id) const { return _blocks_data[id].name; }
    bool isDisplayable(int id) const { return _blocks_data[id].isDisplayable; }
    bool isSolid(int id) const { return _blocks_data[id].isSolid; }
    bool isTransparent(int id) const { return _blocks_data[id].isTransparent; }
    bool isFluid(int id) const { return _blocks_data[id].isFluid; }
    bool isFlammable(int id) const { return _blocks_data[id].isFlammable; }

  private:
    std::vector<BlockData> _blocks_data{MAX_BLOCKS};
};
