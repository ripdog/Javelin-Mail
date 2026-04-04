#include "FixtureReader.h"

#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>

namespace javelin::tests
{

    std::string loadFixture(std::string_view relativePath)
    {
        const std::filesystem::path fixturePath =
            std::filesystem::path{JAVELIN_TEST_DATA_DIR} / relativePath;
        std::ifstream input{fixturePath, std::ios::binary};
        if (!input)
        {
            throw std::runtime_error{"Failed to open fixture: " + fixturePath.string()};
        }

        std::ostringstream buffer;
        buffer << input.rdbuf();
        return buffer.str();
    }

} // namespace javelin::tests
