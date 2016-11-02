/*
 * Copyright (c) 2016, Egor Pugin
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *     1. Redistributions of source code must retain the above copyright
 *        notice, this list of conditions and the following disclaimer.
 *     2. Redistributions in binary form must reproduce the above copyright
 *        notice, this list of conditions and the following disclaimer in the
 *        documentation and/or other materials provided with the distribution.
 *     3. Neither the name of the copyright holder nor the names of
 *        its contributors may be used to endorse or promote products
 *        derived from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY
 * DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 * ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "config.h"

#include "access_table.h"
#include "config.h"
#include "database.h"
#include "directories.h"
#include "hash.h"
#include "hasher.h"
#include "log.h"
#include "templates.h"
#include "stamp.h"

#include <boost/algorithm/string.hpp>

#include "logger.h"
DECLARE_STATIC_LOGGER(logger, "settings");

Settings::Settings()
{
    build_dir = temp_directory_path() / "build";
    storage_dir = get_root_directory() / STORAGE_DIR;
}

void Settings::load(const path &p, const ConfigType type)
{
    auto root = YAML::LoadFile(p.string());
    load(root, type);
}

void Settings::load(const yaml &root, const ConfigType type)
{
    load_main(root, type);

    auto get_storage_dir = [this](ConfigType type)
    {
        switch (type)
        {
        case ConfigType::Local:
            return cppan_dir / STORAGE_DIR;
        case ConfigType::User:
            return Config::get_user_config().settings.storage_dir;
        case ConfigType::System:
            return Config::get_system_config().settings.storage_dir;
        default:
            return storage_dir;
        }
    };

    auto get_build_dir = [this](const path &p, ConfigType type)
    {
        switch (type)
        {
        case ConfigType::Local:
            return fs::current_path();
        case ConfigType::User:
            return directories.storage_dir_tmp;
        case ConfigType::System:
            return temp_directory_path() / "build";
        default:
            return p;
        }
    };

    Directories dirs;
    dirs.storage_dir_type = storage_dir_type;
    dirs.set_storage_dir(get_storage_dir(storage_dir_type));
    dirs.build_dir_type = build_dir_type;
    dirs.set_build_dir(get_build_dir(build_dir, build_dir_type));
    directories.update(dirs, type);
}

void Settings::load_main(const yaml &root, const ConfigType type)
{
    auto packages_dir_type_from_string = [](const String &s, const String &key)
    {
        if (s == "local")
            return ConfigType::Local;
        if (s == "user")
            return ConfigType::User;
        if (s == "system")
            return ConfigType::System;
        throw std::runtime_error("Unknown '" + key + "'. Should be one of [local, user, system]");
    };

    EXTRACT_AUTO(host);
    EXTRACT_AUTO(disable_update_checks);
    EXTRACT(storage_dir, String);
    EXTRACT(build_dir, String);
    EXTRACT(cppan_dir, String);

    auto &p = root["proxy"];
    if (p.IsDefined())
    {
        if (!p.IsMap())
            throw std::runtime_error("'proxy' should be a map");
        EXTRACT_VAR(p, proxy.host, "host", String);
        EXTRACT_VAR(p, proxy.user, "user", String);
    }

    storage_dir_type = packages_dir_type_from_string(get_scalar<String>(root, "storage_dir_type", "user"), "storage_dir_type");
    if (root["storage_dir"].IsDefined())
        storage_dir_type = ConfigType::None;
    build_dir_type = packages_dir_type_from_string(get_scalar<String>(root, "build_dir_type", "system"), "build_dir_type");
    if (root["build_dir"].IsDefined())
        build_dir_type = ConfigType::None;

    // read these first from local settings
    // and they'll be overriden in bs (if they exist there)
    EXTRACT_VAR(root, use_cache, "use_cache", bool);
    EXTRACT_VAR(root, show_ide_projects, "show_ide_projects", bool);
    EXTRACT_VAR(root, add_run_cppan_target, "add_run_cppan_target", bool);

    // read build settings
    if (type == ConfigType::Local)
    {
        // at first, we load bs from current root
        load_build(root);

        // then override settings with specific (or default) config
        yaml current_build;
        if (root["builds"].IsDefined())
        {
            // yaml will not keep sorting of keys in map
            // so we can take 'first' build in document
            if (root["current_build"].IsDefined())
                current_build = root["builds"][root["current_build"].template as<String>()];
        }
        else if (root["build"].IsDefined())
            current_build = root["build"];

        load_build(current_build);
    }
}

void Settings::load_build(const yaml &root)
{
    if (root.IsNull())
        return;

    // extract
    EXTRACT_AUTO(c_compiler);
    EXTRACT_AUTO(cxx_compiler);
    EXTRACT_AUTO(compiler);
    EXTRACT_AUTO(c_compiler_flags);
    if (c_compiler_flags.empty())
        EXTRACT_VAR(root, c_compiler_flags, "c_flags", String);
    EXTRACT_AUTO(cxx_compiler_flags);
    if (cxx_compiler_flags.empty())
        EXTRACT_VAR(root, cxx_compiler_flags, "cxx_flags", String);
    EXTRACT_AUTO(compiler_flags);
    EXTRACT_AUTO(link_flags);
    EXTRACT_AUTO(link_libraries);
    EXTRACT_AUTO(configuration);
    EXTRACT_AUTO(generator);
    EXTRACT_AUTO(toolset);
    EXTRACT_AUTO(type);
    EXTRACT_AUTO(library_type);
    EXTRACT_AUTO(executable_type);
    EXTRACT_AUTO(use_shared_libs);
    EXTRACT_AUTO(silent);
    EXTRACT_AUTO(use_cache);
    EXTRACT_AUTO(show_ide_projects);
    EXTRACT_AUTO(add_run_cppan_target);

    for (int i = 0; i < CMakeConfigurationType::Max; i++)
    {
        auto t = configuration_types[i];
        boost::to_lower(t);
        EXTRACT_VAR(root, c_compiler_flags_conf[i], "c_compiler_flags_" + t, String);
        EXTRACT_VAR(root, cxx_compiler_flags_conf[i], "cxx_compiler_flags_" + t, String);
        EXTRACT_VAR(root, compiler_flags_conf[i], "compiler_flags_" + t, String);
        EXTRACT_VAR(root, link_flags_conf[i], "link_flags_" + t, String);
    }

    cmake_options = get_sequence<String>(root["cmake_options"]);
    get_string_map(root, "env", env);

    // process
    if (c_compiler.empty())
        c_compiler = cxx_compiler;
    if (c_compiler.empty())
        c_compiler = compiler;
    if (cxx_compiler.empty())
        cxx_compiler = compiler;

    c_compiler_flags += " " + compiler_flags;
    cxx_compiler_flags += " " + compiler_flags;
    for (int i = 0; i < CMakeConfigurationType::Max; i++)
    {
        c_compiler_flags_conf[i] += " " + compiler_flags_conf[i];
        cxx_compiler_flags_conf[i] += " " + compiler_flags_conf[i];
    }
}

bool Settings::is_custom_build_dir() const
{
    return build_dir_type == ConfigType::Local || build_dir_type == ConfigType::None;
}

void Settings::set_build_dirs(const path &fn)
{
    filename = fn.filename().string();
    filename_without_ext = fn.filename().stem().string();
    if (filename == CPPAN_FILENAME)
    {
        is_dir = true;
        filename = fn.parent_path().filename().string();
        filename_without_ext = filename;
    }

    source_directory = directories.build_dir;
    if (directories.build_dir_type == ConfigType::Local ||
        directories.build_dir_type == ConfigType::None)
    {
        source_directory /= (CPPAN_LOCAL_BUILD_PREFIX + filename);
    }
    else
    {
        source_directory_hash = sha256(normalize_path(fn.string())).substr(0, 8);
        source_directory /= source_directory_hash;
    }
    binary_directory = source_directory / "build";
}

void Settings::append_build_dirs(const path &p)
{
    source_directory /= p;
    binary_directory = source_directory / "build";
}

void Settings::_prepare_build(Config *c, const path &fn, const String &cppan, bool force) const
{
    auto &p = c->getDefaultProject();
    if (!is_dir)
        p.sources.insert(filename);
    p.findSources(fn.parent_path());
    p.files.erase(CPPAN_FILENAME);

    if (rebuild)
        fs::remove_all(source_directory);
    if (!fs::exists(source_directory))
        fs::create_directories(source_directory);

    write_file_if_different(source_directory / CPPAN_FILENAME, cppan);

    if (!prepare && !force)
        return;

    Config conf(source_directory);
    conf.process(source_directory); // invoke cppan
}

String Settings::get_hash() const
{
    Hasher h;
    h |= c_compiler;
    h |= cxx_compiler;
    h |= compiler;
    h |= c_compiler_flags;
    for (int i = 0; i < CMakeConfigurationType::Max; i++)
        h |= c_compiler_flags_conf[i];
    h |= cxx_compiler_flags;
    for (int i = 0; i < CMakeConfigurationType::Max; i++)
        h |= cxx_compiler_flags_conf[i];
    h |= compiler_flags;
    for (int i = 0; i < CMakeConfigurationType::Max; i++)
        h |= compiler_flags_conf[i];
    h |= link_flags;
    for (int i = 0; i < CMakeConfigurationType::Max; i++)
        h |= link_flags_conf[i];
    h |= link_libraries;
    h |= generator;
    h |= toolset;
    h |= use_shared_libs;
    return h.hash;
}

String Settings::get_fs_generator() const
{
    String g = generator;
    boost::to_lower(g);
    boost::replace_all(g, " ", "-");
    return g;
}

String get_config(const Settings &settings)
{
    auto &db = getServiceDatabase();
    auto h = settings.get_hash();
    auto c = db.getConfigByHash(h);

    if (!c.empty())
        return c;

    c = test_run(settings);
    db.addConfigHash(h, c);

    return c;
}

String test_run(const Settings &settings)
{
    String c;

    // do a test build to extract config string
    auto src_dir = temp_directory_path() / "temp" / fs::unique_path();
    auto bin_dir = src_dir / "build";

    fs::create_directories(src_dir);
    write_file(src_dir / CPPAN_FILENAME, "");
    SCOPE_EXIT
    {
        // remove test dir
        boost::system::error_code ec;
        fs::remove_all(src_dir, ec);
    };

    // invoke cppan
    Config conf(src_dir);
    conf.process(src_dir);
    conf.settings = settings;
    conf.settings.allow_links = false;
    conf.settings.source_directory = src_dir;
    conf.settings.binary_directory = bin_dir;

    auto printer = Printer::create(settings.printerType);
    printer->rc = &conf;
    printer->prepare_build2();

    LOG("--");
    LOG("-- Performing test run");
    LOG("--");

    auto ret = printer->generate();

    if (ret)
        throw std::runtime_error("There are errors during test run");

    // read cfg
    c = read_file(bin_dir / CPPAN_CONFIG_FILENAME);
    auto cmake_version = get_cmake_version();

    // move this to printer some time
    // copy cached cmake config to storage
    copy_dir(
        bin_dir / "CMakeFiles" / cmake_version,
        directories.storage_dir_cfg / c / "CMakeFiles" / cmake_version);

    return c;
}

void Settings::prepare_build(Config *c, path fn, const String &cppan)
{
    fn = fs::canonical(fs::absolute(fn));

    auto printer = Printer::create(printerType);
    printer->rc = c;

    config = get_config(*this);

    // set new dirs
    set_build_dirs(fn);
    append_build_dirs(config);

    auto cmake_version = get_cmake_version();
    auto src = directories.storage_dir_cfg / config / "CMakeFiles" / cmake_version;

    // if dir does not exist it means probably we have new cmake version
    // we have config value but there was not a test run with copying cmake prepared files
    // so start unconditional test run
    if (!fs::exists(src))
        test_run(*this);

    // move this to printer some time
    // copy cached cmake config to bin dir
    copy_dir(src, binary_directory / "CMakeFiles" / cmake_version);

    // TODO: remove?
    // setup cppan config
    _prepare_build(c, fn, cppan);

    // setup printer config
    printer->prepare_build(fn);
}

int Settings::build_package(Config *c)
{
    auto printer = Printer::create(printerType);
    printer->rc = c;

    auto config = get_config(*this);

    source_directory = temp_directory_path() / "temp" / fs::unique_path();
    binary_directory = source_directory / "build";

    auto cmake_version = get_cmake_version();
    auto src = directories.storage_dir_cfg / config / "CMakeFiles" / cmake_version;

    // if dir does not exist it means probably we have new cmake version
    // we have config value but there was not a test run with copying cmake prepared files
    // so start unconditional test run
    if (!fs::exists(src))
        test_run(*this);

    // move this to printer some time
    // copy cached cmake config to bin dir
    copy_dir(src, binary_directory / "CMakeFiles" / cmake_version);

    /*SCOPE_EXIT
    {
        boost::system::error_code ec;
        fs::remove_all(source_directory, ec);
    };*/

    // setup printer config
    c->process(source_directory);
    printer->prepare_build2();

    auto ret = generate(c);
    if (ret)
        return ret;
    return build(c);
}

int Settings::generate(Config *c) const
{
    auto printer = Printer::create(printerType);
    printer->rc = c;
    return printer->generate();
}

int Settings::build(Config *c) const
{
    auto printer = Printer::create(printerType);
    printer->rc = c;
    return printer->build();
}

void Settings::checkForUpdates() const
{
    if (disable_update_checks)
        return;

#ifdef _WIN32
    String stamp_file = "/client/.service/win32.stamp";
#elif __APPLE__
    String stamp_file = "/client/.service/macos.stamp";
#else
    String stamp_file = "/client/.service/linux.stamp";
#endif

    DownloadData dd;
    dd.url = host + stamp_file;
    dd.fn = fs::temp_directory_path() / fs::unique_path();
    download_file(dd);
    auto stamp_remote = boost::trim_copy(read_file(dd.fn));
    boost::replace_all(stamp_remote, "\"", "");
    uint64_t s1 = std::stoull(cppan_stamp);
    uint64_t s2 = std::stoull(stamp_remote);
    if (s1 != 0 && s2 != 0 && s2 > s1)
    {
        std::cout << "New version of the CPPAN client is available!" << "\n";
        std::cout << "Feel free to upgrade it from website or simply run:" << "\n";
        std::cout << "cppan --self-upgrade" << "\n";
#ifdef _WIN32
        std::cout << "(or the same command but from administrator)" << "\n";
#else
        std::cout << "or" << "\n";
        std::cout << "sudo cppan --self-upgrade" << "\n";
#endif
        std::cout << "\n";
    }
}