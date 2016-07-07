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

#include <iostream>
#include <string>

#ifdef _WIN32
#include <windows.h>
#endif

#include <boost/algorithm/string.hpp>

#include <cppan.h>
#include "options.h"

void self_upgrade(Config &c, const char *exe_path);

int main(int argc, char *argv[])
try
{
    // default run
    if (argc == 1)
    {
        auto c = Config::load_user_config();
        c.load_current_config();
        c.download_dependencies();
        c.create_build_files();
        return 0;
    }

    // command selector
    if (argv[1][0] != '-')
    {
        // config
        return 1;
    }
    else if (String(argv[1]) == "--self-upgrade-copy")
    {
#ifndef _WIN32
        std::cout << "This feature is not supported on this platform" << "\n";
        return 1;
#endif
        // self upgrade via copy
        fs::copy_file(argv[0], argv[2], fs::copy_option::overwrite_if_exists);
        return 0;
    }

    // default command run

    ProgramOptions options;
    bool r = options.parseArgs(argc, argv);

    // always first
    if (!r || options().count("help"))
    {
        std::cout << options.printHelp() << "\n";
        return !r;
    }
    if (options["version"].as<bool>())
    {
        std::cout << get_program_version_string("cppan") << "\n";
        return 0;
    }

    // set correct working directory to look for config file
    if (options().count("dir"))
        fs::current_path(options["dir"].as<std::string>());
    httpSettings.verbose = options["curl-verbose"].as<bool>();

    auto c = Config::load_user_config();

    // setup curl settings if possible from config
    // other network users (options) should go below this line
    httpSettings.proxy = c.proxy;

    // self-upgrade?
    if (options()["self-upgrade"].as<bool>())
    {
        self_upgrade(c, argv[0]);
        return 0;
    }

    // load config from current dir
    c.load_current_config();

    // update proxy settings?
    httpSettings.proxy = c.proxy;

    if (options()["prepare-archive"].as<bool>())
    {
        Projects &projects = c.getProjects();
        for (auto &project : projects)
        {
            project.findSources(".");
            String archive_name = make_archive_name(project.package.toString());
            if (!project.writeArchive(archive_name))
                throw std::runtime_error("Archive write failed");
        }
    }
    else
    {
        c.download_dependencies();
        c.create_build_files();
    }

    return 0;
}
catch (const std::exception &e)
{
    std::cout << e.what() << "\n";
    return 1;
}
catch (...)
{
    std::cout << "Unhandled unknown exception" << "\n";
    return 1;
}

void self_upgrade(Config &c, const char *exe_path)
{
#ifndef _WIN32
    std::cout << "This feature is not supported on this platform" << "\n";
    return;
#endif

    String client = "/client/cppan-master-win32-client.zip";

    DownloadData dd;
    dd.url = c.host + client + ".md5";
    dd.fn = fs::temp_directory_path() / fs::unique_path();
    std::cout << "Downloading checksum file" << "\n";
    download_file(dd);
    auto md5 = boost::algorithm::trim_copy(read_file(dd.fn));

    dd.url = c.host + client;
    dd.fn = fs::temp_directory_path() / fs::unique_path();
    String dl_md5;
    dd.dl_md5 = &dl_md5;
    std::cout << "Downloading the latest client" << "\n";
    download_file(dd);
    if (md5 != dl_md5)
        throw std::runtime_error("Downloaded bad file (md5 check failed)");

    std::cout << "Unpacking" << "\n";
    unpack_file(dd.fn, fs::temp_directory_path());

    // self update
    auto exe = (fs::temp_directory_path() / "cppan.exe").wstring();
    auto arg0 = L"\"" + exe + L"\"";
    WCHAR fn[1024] = { 0 };
    GetModuleFileNameW(NULL, fn, sizeof(fn) * sizeof(WCHAR));
    auto dst = L"\"" + std::wstring(fn) + L"\"";
    std::cout << "Replacing client" << "\n";
    if (_wexecl(exe.c_str(), arg0.c_str(), L"--self-upgrade-copy", dst.c_str(), 0) == -1)
    {
        throw std::runtime_error(String("errno = ") + std::to_string(errno) + "\n" +
            "Cannot do a self upgrade. Replace this file with newer CPPAN client manually.");
    }
}
