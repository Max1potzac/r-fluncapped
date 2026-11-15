#pragma once

// Reflourished's FilePickerHelper (1.5.0+): the game asks Android's document picker for a level
// or a save to import, or for where to export one. On the PC the picking is the desktop's own
// Open / Save as window; these do the copying FilePickerHelper does around the picker, between the
// picked file and the game's external files folder (getExternalFilesDir(null)).

#include <filesystem>
#include <optional>
#include <string>

namespace rfu::android::file_picker {

// `files` is the external files folder on the host; `rel` is the path the game gave, under it.
// Each returns what it wrote, or nullopt (and logs why) when it couldn't.

// importFile(newName): the picked file becomes files/rel.
std::optional<std::filesystem::path> import_file(const std::filesystem::path& picked, const std::filesystem::path& files, const std::string& rel);
// importFileToDir(destDirPath): the picked file goes into files/rel/ under its own name.
std::optional<std::filesystem::path> import_file_to_dir(const std::filesystem::path& picked, const std::filesystem::path& files, const std::string& rel);
// importZip(destParentPath): the picked zip is unpacked into files/rel/; each folder at the top of
// the zip replaces the one of that name already there.
std::optional<std::filesystem::path> import_zip(const std::filesystem::path& picked, const std::filesystem::path& files, const std::string& rel);
// exportFile / exportFolder: the name the save window suggests ("<name>_yyyyMMdd_HHmmss.<ext>",
// "<folder>_yyyyMMdd_HHmmss.zip"), and the copying once a place is picked.
std::string export_file_name(const std::string& rel);
std::string export_folder_name(const std::string& rel);
bool export_file(const std::filesystem::path& files, const std::string& rel, const std::filesystem::path& to);
bool export_folder(const std::filesystem::path& files, const std::string& rel, const std::filesystem::path& to);

// What the game is importing or exporting, from the path it gave, for the window's title
// ("custom level", "save", "quest progress", or "file"), and the file type to list when importing
// a single file ("json" for a level; empty for every file).
std::string what(const std::string& rel);
std::string import_extension(const std::string& rel);

// files/rel, or nullopt if `rel` would leave `files` ("../..").
std::optional<std::filesystem::path> inside(const std::filesystem::path& files, const std::string& rel);

}  // namespace rfu::android::file_picker
