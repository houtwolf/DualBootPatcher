/*
 * Copyright (C) 2015  Andrew Gunnerson <andrewgunnerson@gmail.com>
 *
 * This file is part of DualBootPatcher
 *
 * DualBootPatcher is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * DualBootPatcher is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with DualBootPatcher.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "utilities.h"

#include <algorithm>

#include <cstring>

#include <fcntl.h>
#include <getopt.h>
#include <sys/stat.h>
#include <unistd.h>

#include "minizip/zip.h"
#include "minizip/ioandroid.h"

#include "mbcommon/string.h"
#include "mbcommon/version.h"
#include "mbdevice/device.h"
#include "mbdevice/json.h"
#include "mblog/logging.h"
#include "mblog/stdio_logger.h"
#include "mbutil/delete.h"
#include "mbutil/file.h"
#include "mbutil/fts.h"
#include "mbutil/path.h"
#include "mbutil/properties.h"
#include "mbutil/string.h"

#include "multiboot.h"
#include "romconfig.h"
#include "roms.h"
#include "switcher.h"
#include "wipe.h"

#define LOG_TAG "mbtool/utilities"

using namespace mb::device;

namespace mb
{

static const char *devices_file = nullptr;

static bool get_device(const char *path, Device &device)
{
    std::string prop_product_device =
            util::property_get_string("ro.product.device", {});
    std::string prop_build_product =
            util::property_get_string("ro.build.product", {});

    LOGD("ro.product.device = %s", prop_product_device.c_str());
    LOGD("ro.build.product = %s", prop_build_product.c_str());

    std::vector<unsigned char> contents;
    if (!util::file_read_all(path, contents)) {
        LOGE("%s: Failed to read file: %s", path, strerror(errno));
        return false;
    }
    contents.push_back('\0');

    std::vector<Device> devices;
    JsonError error;

    if (!device_list_from_json(reinterpret_cast<const char *>(contents.data()),
                               devices, error)) {
        LOGE("%s: Failed to load devices", path);
        return false;
    }

    for (auto &d : devices) {
        if (d.validate()) {
            LOGW("Skipping invalid device");
            continue;
        }

        auto const &codenames = d.codenames();
        auto it = std::find_if(codenames.begin(), codenames.end(),
                               [&](const std::string &item) {
            return item == prop_product_device || item == prop_build_product;
        });

        if (it != codenames.end()) {
            device = std::move(d);
            return true;
        }
    }

    LOGE("Unknown device: %s", prop_product_device.c_str());
    return false;
}

static bool utilities_switch_rom(const char *rom_id, bool force)
{
    Device device;

    if (!devices_file) {
        LOGE("No device definitions file specified");
        return false;
    }

    if (!get_device(devices_file, device)) {
        LOGE("Failed to detect device");
        return false;
    }

    auto const &boot_devs = device.boot_block_devs();
    auto it = std::find_if(boot_devs.begin(), boot_devs.end(),
                           [&](const std::string &path) {
        struct stat sb;
        return stat(path.c_str(), &sb) == 0 && S_ISBLK(sb.st_mode);
    });

    if (it == boot_devs.end()) {
        LOGE("All specified boot partition paths could not be found");
        return false;
    }

    SwitchRomResult ret = switch_rom(
            rom_id, *it, device.block_dev_base_dirs(), force);
    switch (ret) {
    case SwitchRomResult::Succeeded:
        LOGD("SUCCEEDED");
        break;
    case SwitchRomResult::Failed:
        LOGD("FAILED");
        break;
    case SwitchRomResult::ChecksumInvalid:
        LOGD("CHECKSUM_INVALID");
        break;
    case SwitchRomResult::ChecksumNotFound:
        LOGD("CHECKSUM_NOT_FOUND");
        break;
    }

    return ret == SwitchRomResult::Succeeded;
}

static bool utilities_wipe_system(const char *rom_id)
{
    auto rom = Roms::create_rom(rom_id);
    if (!rom) {
        return false;
    }

    return wipe_system(rom);
}

static bool utilities_wipe_cache(const char *rom_id)
{
    auto rom = Roms::create_rom(rom_id);
    if (!rom) {
        return false;
    }

    return wipe_cache(rom);
}

static bool utilities_wipe_data(const char *rom_id)
{
    auto rom = Roms::create_rom(rom_id);
    if (!rom) {
        return false;
    }

    return wipe_data(rom);
}

static bool utilities_wipe_dalvik_cache(const char *rom_id)
{
    auto rom = Roms::create_rom(rom_id);
    if (!rom) {
        return false;
    }

    return wipe_dalvik_cache(rom);
}

static bool utilities_wipe_multiboot(const char *rom_id)
{
    auto rom = Roms::create_rom(rom_id);
    if (!rom) {
        return false;
    }

    return wipe_multiboot(rom);
}

static void generate_aroma_config(std::vector<unsigned char> *data)
{
    std::string str_data(data->begin(), data->end());

    std::string rom_menu_items;
    std::string rom_selection_items;

    Roms roms;
    roms.add_installed();

    for (std::size_t i = 0; i < roms.roms.size(); ++i) {
        const std::shared_ptr<Rom> &rom = roms.roms[i];

        std::string config_path = rom->config_path();
        std::string name = rom->id;

        RomConfig config;
        if (config.load_file(config_path) && !config.name.empty()) {
            name = config.name;
        }

        rom_menu_items += format("\"%s\", \"\", \"@default\",\n", name.c_str());

        rom_selection_items += format(
                "if prop(\"operations.prop\", \"selected\") == \"%zu\" then\n"
                "    setvar(\"romid\", \"%s\");\n"
                "    setvar(\"romname\", \"%s\");\n"
                "endif;\n",
                i + 2 + 1, rom->id.c_str(), name.c_str());
    }

    std::string first_index = format("%d", 2 + 1);
    std::string last_index = format("%zu", 2 + roms.roms.size());

    util::replace_all(str_data, "\t", "\\t");
    util::replace_all(str_data, "@MBTOOL_VERSION@", version());
    util::replace_all(str_data, "@ROM_MENU_ITEMS@", rom_menu_items);
    util::replace_all(str_data, "@ROM_SELECTION_ITEMS@", rom_selection_items);
    util::replace_all(str_data, "@FIRST_INDEX@", first_index);
    util::replace_all(str_data, "@LAST_INDEX@", last_index);

    util::replace_all(str_data, "@SYSTEM_MOUNT_POINT@", Roms::get_system_partition());
    util::replace_all(str_data, "@CACHE_MOUNT_POINT@", Roms::get_cache_partition());
    util::replace_all(str_data, "@DATA_MOUNT_POINT@", Roms::get_data_partition());
    util::replace_all(str_data, "@EXTSD_MOUNT_POINT@", Roms::get_extsd_partition());

    data->assign(str_data.begin(), str_data.end());
}

class AromaGenerator : public util::FtsWrapper
{
public:
    AromaGenerator(std::string path, std::string zippath)
        : FtsWrapper(std::move(path), util::FtsFlag::GroupSpecialFiles),
        _zippath(std::move(zippath))
    {
    }

    bool on_pre_execute() override
    {
        zlib_filefunc64_def zFunc;
        memset(&zFunc, 0, sizeof(zFunc));
        fill_android_filefunc64(&zFunc);
        _zf = zipOpen2_64(_zippath.c_str(), 0, nullptr, &zFunc);
        if (!_zf) {
            LOGE("%s: Failed to open for writing", _zippath.c_str());
            return false;
        }

        return true;
    }

    bool on_post_execute(bool success) override
    {
        (void) success;
        return zipClose(_zf, nullptr) == ZIP_OK;
    }

    Actions on_reached_file() override
    {
        std::string name = std::string(_curr->fts_path).substr(_path.size() + 1);
        LOGD("%s -> %s", _curr->fts_path, name.c_str());

        if (name == "META-INF/com/google/android/aroma-config.in") {
            std::vector<unsigned char> data;
            if (!util::file_read_all(_curr->fts_accpath, data)) {
                LOGE("Failed to read: %s", _curr->fts_path);
                return Action::Fail;
            }

            generate_aroma_config(&data);

            name = "META-INF/com/google/android/aroma-config";
            bool ret = add_file(name, data);
            return ret ? Action::Ok : Action::Fail;
        } else {
            bool ret = add_file(name, _curr->fts_accpath);
            return ret ? Action::Ok : Action::Fail;
        }
    }

    Actions on_reached_symlink() override
    {
        LOGW("Ignoring symlink when creating zip: %s", _curr->fts_path);
        return Action::Ok;
    }

    Actions on_reached_special_file() override
    {
        LOGW("Ignoring special file when creating zip: %s", _curr->fts_path);
        return Action::Ok;
    }

private:
    zipFile _zf;
    std::string _zippath;

    bool add_file(const std::string &name,
                  const std::vector<unsigned char> &contents)
    {
        // Obviously never true, but we'll keep it here just in case
        bool zip64 = static_cast<uint64_t>(contents.size())
                >= ((1ull << 32) - 1);

        zip_fileinfo zi;
        memset(&zi, 0, sizeof(zi));

        int ret = zipOpenNewFileInZip2_64(
            _zf,                    // file
            name.c_str(),           // filename
            &zi,                    // zip_fileinfo
            nullptr,                // extrafield_local
            0,                      // size_extrafield_local
            nullptr,                // extrafield_global
            0,                      // size_extrafield_global
            nullptr,                // comment
            Z_DEFLATED,             // method
            Z_DEFAULT_COMPRESSION,  // level
            0,                      // raw
            zip64                   // zip64
        );

        if (ret != ZIP_OK) {
            LOGW("minizip: Failed to add file (error code: %d): [memory]", ret);

            return false;
        }

        // Write data to file
        ret = zipWriteInFileInZip(_zf, contents.data(),
                                  static_cast<uint32_t>(contents.size()));
        if (ret != ZIP_OK) {
            LOGW("minizip: Failed to write data (error code: %d): [memory]", ret);
            zipCloseFileInZip(_zf);

            return false;
        }

        zipCloseFileInZip(_zf);

        return true;
    }

    bool add_file(const std::string &name, const std::string &path)
    {
        // Copy file into archive
        int fd = open64(path.c_str(), O_RDONLY);
        if (fd < 0) {
            LOGE("%s: Failed to open for reading: %s",
                 path.c_str(), strerror(errno));
            return false;
        }

        struct stat sb;
        if (fstat(fd, &sb) < 0) {
            LOGE("%s: Failed to stat: %s",
                 path.c_str(), strerror(errno));
            return false;
        }

        off64_t size;
        lseek64(fd, 0, SEEK_END);
        size = lseek64(fd, 0, SEEK_CUR);
        lseek64(fd, 0, SEEK_SET);

        if (size < 0) {
            LOGE("%s: Failed to seek: %s",
                 path.c_str(), strerror(errno));
            return false;
        }

        bool zip64 = static_cast<uint64_t>(size) >= ((1ull << 32) - 1);

        zip_fileinfo zi;
        memset(&zi, 0, sizeof(zi));
        zi.external_fa = (sb.st_mode & 0777) << 16;

        int ret = zipOpenNewFileInZip2_64(
            _zf,                    // file
            name.c_str(),           // filename
            &zi,                    // zip_fileinfo
            nullptr,                // extrafield_local
            0,                      // size_extrafield_local
            nullptr,                // extrafield_global
            0,                      // size_extrafield_global
            nullptr,                // comment
            Z_DEFLATED,             // method
            Z_DEFAULT_COMPRESSION,  // level
            0,                      // raw
            zip64                   // zip64
        );

        if (ret != ZIP_OK) {
            LOGW("minizip: Failed to add file (error code: %d): %s",
                 ret, path.c_str());
            return false;
        }

        // Write data to file
        char buf[32768];
        ssize_t n;

        while ((n = read(fd, buf, sizeof(buf))) > 0) {
            ret = zipWriteInFileInZip(_zf, buf, static_cast<uint32_t>(n));
            if (ret != ZIP_OK) {
                LOGW("minizip: Failed to write data (error code: %d): %s",
                     ret, path.c_str());
                zipCloseFileInZip(_zf);

                return false;
            }
        }

        if (n < 0) {
            zipCloseFileInZip(_zf);

            LOGE("%s: Failed to read file: %s",
                 path.c_str(), strerror(errno));
            return false;
        }

        zipCloseFileInZip(_zf);

        return true;
    }
};

static void utilities_usage(bool error)
{
    FILE *stream = error ? stderr : stdout;

    fprintf(stream,
            "Usage: utilities [opt...] generate [template dir] [output file]\n"
            "   OR: utilities [opt...] switch [ROM ID] [--force]\n"
            "   OR: utilities [opt...] wipe-system [ROM ID]\n"
            "   OR: utilities [opt...] wipe-cache [ROM ID]\n"
            "   OR: utilities [opt...] wipe-data [ROM ID]\n"
            "   OR: utilities [opt...] wipe-dalvik-cache [ROM ID]\n"
            "   OR: utilities [opt...] wipe-multiboot [ROM ID]\n"
            "\n"
            "Options:\n"
            "  -f, --force      Force (only for 'switch' action)\n"
            "  -d, --devices    Path to device defintions file\n");
}

int utilities_main(int argc, char *argv[])
{
    // Make stdout unbuffered
    setvbuf(stdout, nullptr, _IONBF, 0);

    log::set_logger(std::make_shared<log::StdioLogger>(stdout));

    bool force = false;

    int opt;

    static struct option long_options[] = {
        {"help", no_argument, 0, 'h'},
        {"force", no_argument, 0, 'f'},
        {"devices", required_argument, 0, 'd'},
        {0, 0, 0, 0}
    };

    int long_index = 0;

    while ((opt = getopt_long(argc, argv, "hfd:",
                              long_options, &long_index)) != -1) {
        switch (opt) {
        case 'f':
            force = true;
            break;

        case 'd':
            devices_file = optarg;
            break;

        case 'h':
            utilities_usage(false);
            return EXIT_SUCCESS;

        default:
            utilities_usage(true);
            return EXIT_FAILURE;
        }
    }

    if (argc - optind == 0) {
        utilities_usage(true);
        return EXIT_FAILURE;
    }

    const std::string action = argv[optind];
    if ((action == "generate" && argc - optind != 3)
            || (action != "generate" && argc - optind != 2)) {
        utilities_usage(true);
        return EXIT_FAILURE;
    }

    if (force && action != "switch") {
        utilities_usage(true);
        return EXIT_FAILURE;
    }

    bool ret = false;

    if (action == "generate") {
        AromaGenerator gen(argv[optind + 1], argv[optind + 2]);
        ret = gen.run();
    } else if (action == "switch") {
        ret = utilities_switch_rom(argv[optind + 1], force);
    } else if (action == "wipe-system") {
        ret = utilities_wipe_system(argv[optind + 1]);
    } else if (action == "wipe-cache") {
        ret = utilities_wipe_cache(argv[optind + 1]);
    } else if (action == "wipe-data") {
        ret = utilities_wipe_data(argv[optind + 1]);
    } else if (action == "wipe-dalvik-cache") {
        ret = utilities_wipe_dalvik_cache(argv[optind + 1]);
    } else if (action == "wipe-multiboot") {
        ret = utilities_wipe_multiboot(argv[optind + 1]);
    } else {
        LOGE("Unknown action: %s", action.c_str());
    }

    return ret ? EXIT_SUCCESS : EXIT_FAILURE;
}

}
