#pragma once

// The desktop's own "Open" and "Save as" windows, for when the game asks Android's document
// picker for a file (Reflourished's level and save import/export). Both block until the person
// picks a file or cancels, and give nullopt for a cancel or when there's no dialog to show.

#include <filesystem>
#include <optional>
#include <string>

namespace rfu::platform {

// `extension` narrows the list to one kind of file ("zip"); empty shows every file.
std::optional<std::filesystem::path> open_file_dialog(const std::string& title, const std::string& extension = {});
std::optional<std::filesystem::path> save_file_dialog(const std::string& title, const std::string& suggested_name);

}  // namespace rfu::platform
