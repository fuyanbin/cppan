#include "cmake.h"

#include "../access_table.h"
#include "../inserts.h"
#include "../log.h"
#include "../response.h"

#ifdef _WIN32
#include "shell_link.h"
#endif

#include <boost/algorithm/string.hpp>

// common?
const String cppan_dummy_target = "cppan-dummy";
const String cppan_helpers_target = "cppan-helpers";
const String cppan_helpers_private_target = "cppan-helpers-private";
const String exports_dir = "${CMAKE_BINARY_DIR}/exports/";
const String packages_folder = "cppan/packages";

//
const String cmake_config_filename = "CMakeLists.txt";
const String actions_filename = "actions.cmake";
const String non_local_build_file = "build.cmake";
const String exports_filename = "exports.cmake";
const String cmake_functions_filename = "functions.cmake";
const String cmake_object_config_filename = "generate.cmake";
const String cmake_helpers_filename = "helpers.cmake";
const String include_guard_filename = "include.cmake";
const String cppan_stamp_filename = "cppan_sources.stamp";
const String cmake_minimum_required = "cmake_minimum_required(VERSION 3.2.0)";

String repeat(const String &e, int n)
{
    String s;
    s.reserve(e.size() * n);
    for (int i = 0; i < n; i++)
        s += e;
    return s;
}

const String config_delimeter_short = repeat("#", 40);
const String config_delimeter = config_delimeter_short + config_delimeter_short;

void config_section_title(Context &ctx, const String &t)
{
    ctx.emptyLines(1);
    ctx.addLine(config_delimeter);
    ctx.addLine("#");
    ctx.addLine("# " + t);
    ctx.addLine("#");
    ctx.addLine(config_delimeter);
    ctx.emptyLines(1);
}

void file_title(Context &ctx, const Package &d)
{
    ctx.addLine("#");
    ctx.addLine("# cppan");
    ctx.addLine("# package: " + d.ppath.toString());
    ctx.addLine("# version: " + d.version.toString());
    ctx.addLine("#");
    ctx.addLine("# source dir: " + normalize_path(d.getDirSrc().string()));
    ctx.addLine("# binary dir: " + normalize_path(d.getDirObj().string()));
    ctx.addLine("# package hash: " + d.getHash());
    ctx.addLine("#");
    ctx.addLine();
}

String add_subdirectory(String src, String bin = String())
{
    boost::algorithm::replace_all(src, "\\", "/");
    boost::algorithm::replace_all(bin, "\\", "/");
    return "include(\"" + src + "/" + include_guard_filename + "\")";
}

void add_subdirectory(Context &ctx, const String &src, const String &bin = String())
{
    ctx << add_subdirectory(src, bin) << Context::eol;
}

String get_binary_path(const Package &d)
{
    return "${CMAKE_BINARY_DIR}/cppan/" + d.getHash();
}

void print_dependencies(Context &ctx, const Packages &dd, bool local_build)
{
    std::vector<String> includes;

    if (dd.empty())
        return;

    bool obj_dir = true;
    if (local_build)
        obj_dir = false;

    auto base_dir = directories.storage_dir_src;
    if (obj_dir)
        base_dir = directories.storage_dir_obj;

    config_section_title(ctx, "direct dependencies");
    for (auto &p : dd)
    {
        String s;
        auto dir = base_dir;
        // do not "optimize" this condition
        if (p.second.flags[pfHeaderOnly] ||
            p.second.flags[pfIncludeDirectories])
        {
            dir = directories.storage_dir_src;
            s = p.second.getDirSrc().string();
        }
        else if (obj_dir)
        {
            s = p.second.getDirObj().string();
        }
        else
        {
            dir = directories.storage_dir_src;
            s = p.second.getDirSrc().string();
        }

        if (p.second.flags[pfIncludeDirectories])
        {
            // MUST be here!
            // actions are executed from include_directories only projects
            ctx.addLine("include(\"" + normalize_path(s) + "/" + actions_filename + "\")");
        }
        else if (local_build || p.second.flags[pfHeaderOnly])
        {
            ctx.addLine("# " + p.second.target_name);
            add_subdirectory(ctx, s, get_binary_path(p.second));
        }
        else
        {
            includes.push_back("include(\"" + normalize_path(s) + "/" + cmake_object_config_filename + "\")");
        }
    }
    ctx.addLine();

    if (!includes.empty())
    {
        config_section_title(ctx, "include dependencies (they should be placed at the end)");
        for (auto &line : includes)
            ctx.addLine(line);
    }
}

void print_source_groups(Context &ctx, const path &dir)
{
    bool once = false;
    for (auto &f : boost::make_iterator_range(fs::recursive_directory_iterator(dir), {}))
    {
        if (!fs::is_directory(f))
            continue;

        if (!once)
            config_section_title(ctx, "source groups");
        once = true;

        auto s = fs::relative(f.path(), dir).string();
        auto s2 = boost::replace_all_copy(s, "\\", "\\\\");
        boost::replace_all(s2, "/", "\\\\");

        ctx.addLine("source_group(\"" + s2 + "\" FILES");
        ctx.increaseIndent();
        for (auto &f2 : boost::make_iterator_range(fs::directory_iterator(f), {}))
        {
            if (!fs::is_regular_file(f2))
                continue;
            ctx.addLine("\"" + normalize_path(f2.path()) + "\"");
        }
        ctx.decreaseIndent();
        ctx.addLine(")");
    }
    ctx.emptyLines(1);
}

void gather_build_deps(Context &ctx, const Packages &dd, Packages &out)
{
    for (auto &dp : dd)
    {
        auto &d = dp.second;
        if (d.flags[pfExecutable] || d.flags[pfHeaderOnly] || d.flags[pfIncludeDirectories])
            continue;
        auto i = out.insert(dp);
        if (i.second)
            gather_build_deps(ctx, rd[d].dependencies, out);
    }
}

void CMakePrinter::prepare_rebuild()
{
    // remove stamp file to start rebuilding
    auto odir = d.getDirObj() / "build";
    if (!fs::exists(odir))
        return;
    for (auto &dir : boost::make_iterator_range(fs::directory_iterator(), {}))
    {
        if (!fs::is_directory(dir))
            continue;
        for (auto &f : boost::make_iterator_range(fs::directory_iterator(dir), {}))
        {
            if (!fs::is_regular_file(f))
                continue;
            if (f.path().filename().string() != cppan_stamp_filename)
                continue;
            remove_file(f);
        }
    }
}

void CMakePrinter::prepare_build(const path &fn, const String &cppan)
{
    auto &build_settings = rc->local_settings.build_settings;
    auto &p = rc->getDefaultProject();

    Context ctx;
    config_section_title(ctx, "cmake settings");
    ctx.addLine(cmake_minimum_required);
    ctx.addLine();

    config_section_title(ctx, "project settings");
    ctx.addLine("project(" + build_settings.filename_without_ext + " C CXX)");
    ctx.addLine();

    config_section_title(ctx, "compiler & linker settings");
    ctx.addLine(R"(# Output directory settings
set(output_dir ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${output_dir})

if (NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

if (MSVC)
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} /MP")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} /MP")
endif()
)");

    // compiler flags
    ctx.addLine("set(CMAKE_C_FLAGS \"${CMAKE_C_FLAGS} " + build_settings.c_compiler_flags + "\")");
    ctx.addLine("set(CMAKE_CXX_FLAGS \"${CMAKE_CXX_FLAGS} " + build_settings.cxx_compiler_flags + "\")");
    ctx.addLine();

    for (int i = 0; i < BuildSettings::CMakeConfigurationType::Max; i++)
    {
        auto &cfg = configuration_types[i];
        ctx.addLine("set(CMAKE_C_FLAGS_" + cfg + " \"${CMAKE_C_FLAGS_" + cfg + "} " + build_settings.c_compiler_flags_conf[i] + "\")");
        ctx.addLine("set(CMAKE_CXX_FLAGS_" + cfg + " \"${CMAKE_CXX_FLAGS_" + cfg + "} " + build_settings.cxx_compiler_flags_conf[i] + "\")");
        ctx.addLine();
    }

    // linker flags
    ctx.addLine("set(CMAKE_EXE_LINKER_FLAGS \"${CMAKE_EXE_LINKER_FLAGS} " + build_settings.link_flags + "\")");
    ctx.addLine("set(CMAKE_MODULE_LINKER_FLAGS \"${CMAKE_MODULE_LINKER_FLAGS} " + build_settings.link_flags + "\")");
    ctx.addLine("set(CMAKE_SHARED_LINKER_FLAGS \"${CMAKE_SHARED_LINKER_FLAGS} " + build_settings.link_flags + "\")");
    ctx.addLine("set(CMAKE_STATIC_LINKER_FLAGS \"${CMAKE_STATIC_LINKER_FLAGS} " + build_settings.link_flags + "\")");
    ctx.addLine();

    for (int i = 0; i < BuildSettings::CMakeConfigurationType::Max; i++)
    {
        auto &cfg = configuration_types[i];
        ctx.addLine("set(CMAKE_EXE_LINKER_FLAGS_" + cfg + " \"${CMAKE_EXE_LINKER_FLAGS_" + cfg + "} " + build_settings.link_flags_conf[i] + "\")");
        ctx.addLine("set(CMAKE_MODULE_LINKER_FLAGS_" + cfg + " \"${CMAKE_MODULE_LINKER_FLAGS_" + cfg + "} " + build_settings.link_flags_conf[i] + "\")");
        ctx.addLine("set(CMAKE_SHARED_LINKER_FLAGS_" + cfg + " \"${CMAKE_SHARED_LINKER_FLAGS_" + cfg + "} " + build_settings.link_flags_conf[i] + "\")");
        ctx.addLine("set(CMAKE_STATIC_LINKER_FLAGS_" + cfg + " \"${CMAKE_STATIC_LINKER_FLAGS_" + cfg + "} " + build_settings.link_flags_conf[i] + "\")");
        ctx.addLine();
    }

    // should be after flags
    config_section_title(ctx, "CPPAN include");
    ctx.addLine("set(CPPAN_BUILD_OUTPUT_DIR \"" + normalize_path(fs::current_path()) + "\")");
    if (build_settings.use_shared_libs)
        ctx.addLine("set(CPPAN_BUILD_SHARED_LIBS 1)");
    ctx.addLine("add_subdirectory(cppan)");
    ctx.addLine();

    // add GLOB later
    config_section_title(ctx, "sources");
    ctx.addLine("set(src");
    ctx.increaseIndent();
    for (auto &s : p.files)
        ctx.addLine("\"" + normalize_path(fn.parent_path() / s) + "\"");
    ctx.decreaseIndent();
    ctx.addLine(")");
    ctx.addLine();

    config_section_title(ctx, "target");
    ctx.addLine("set(this " + build_settings.filename_without_ext + ")");
    if (build_settings.type == "executable")
    {
        ctx.addLine("add_executable(${this} " + boost::to_upper_copy(build_settings.executable_type) + " ${src})");
        ctx.addLine("target_compile_definitions(${this} PRIVATE CPPAN_EXPORT=)");
    }
    else
    {
        if (build_settings.type == "library")
        {
            ctx.addLine("add_library(${this} " + boost::to_upper_copy(build_settings.library_type) + " ${src})");
        }
        else
        {
            ctx.addLine("add_library(${this} " + boost::to_upper_copy(build_settings.type) + " ${src})");
        }
        ctx.addLine("target_compile_definitions(${this} PRIVATE CPPAN_EXPORT=CPPAN_SYMBOL_EXPORT)");
        ctx.addLine(R"(set_target_properties(${this} PROPERTIES
    INSTALL_RPATH .
    BUILD_WITH_INSTALL_RPATH True
))");
    }
    ctx.addLine("target_link_libraries(${this} cppan " + build_settings.link_libraries + ")");
    ctx.addLine();
    ctx.addLine(R"(add_custom_command(TARGET ${this} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:${this}> )" + normalize_path(fs::current_path()) + R"(/
))");
    ctx.addLine();

    // eof
    ctx.addLine(config_delimeter);
    ctx.addLine();
    ctx.splitLines();

    write_file_if_different(build_settings.source_directory / cmake_config_filename, ctx.getText());
}

int CMakePrinter::generate() const
{
    auto &build_settings = rc->local_settings.build_settings;

    std::vector<String> args;
    args.push_back("cmake");
    args.push_back("-H\"" + normalize_path(build_settings.source_directory) + "\"");
    args.push_back("-B\"" + normalize_path(build_settings.binary_directory) + "\"");
    if (!build_settings.c_compiler.empty())
        args.push_back("-DCMAKE_C_COMPILER=\"" + build_settings.c_compiler + "\"");
    if (!build_settings.cxx_compiler.empty())
        args.push_back("-DCMAKE_CXX_COMPILER=\"" + build_settings.cxx_compiler + "\"");
    if (!build_settings.generator.empty())
        args.push_back("-G \"" + build_settings.generator + "\"");
    if (!build_settings.toolset.empty())
        args.push_back("-T " + build_settings.toolset + "");
    args.push_back("-DCMAKE_BUILD_TYPE=" + build_settings.configuration + "");
    for (auto &o : build_settings.cmake_options)
        args.push_back(o);
    for (auto &o : build_settings.env)
    {
#ifdef _WIN32
        _putenv_s(o.first.c_str(), o.second.c_str());
#else
        setenv(o.first.c_str(), o.second.c_str(), 1);
#endif
    }
    auto ret = system(args);
    if (!build_settings.silent)
    {
        auto bld_dir = fs::current_path();
#ifdef _WIN32
        auto sln = build_settings.binary_directory / (build_settings.filename_without_ext + ".sln");
        auto sln_new = bld_dir / (build_settings.filename_without_ext + ".sln.lnk");
        if (fs::exists(sln))
            CreateLink(sln.string().c_str(), sln_new.string().c_str(), "Link to CPPAN Solution");
#else
        bld_dir /= path(CPPAN_LOCAL_BUILD_PREFIX + build_settings.filename);
        fs::create_directories(bld_dir);
        boost::system::error_code ec;
        fs::create_symlink(build_settings.source_directory / cmake_config_filename, bld_dir / cmake_config_filename, ec);
#endif
    }
    return ret;
}

int CMakePrinter::build() const
{
    auto &build_settings = rc->local_settings.build_settings;

    std::vector<String> args;
    args.push_back("cmake");
    args.push_back("--build \"" + normalize_path(build_settings.binary_directory) + "\"");
    args.push_back("--config " + build_settings.configuration);
    return system(args);
}

void CMakePrinter::clear_cache(path p) const
{
    if (p.empty())
        p = directories.storage_dir_obj;

    // projects
    for (auto &fp : boost::make_iterator_range(fs::directory_iterator(p), {}))
    {
        if (!fs::is_directory(fp))
            continue;

        // versions
        for (auto &fv : boost::make_iterator_range(fs::directory_iterator(fp), {}))
        {
            if (!fs::is_directory(fv) || !fs::exists(fv / "build"))
                continue;

            // configs
            for (auto &fc : boost::make_iterator_range(fs::directory_iterator(fv / "build"), {}))
            {
                if (!fs::is_directory(fc))
                    continue;
                remove_file(fc / "CMakeCache.txt");
            }
        }
    }

    clear_exports(p);
}

void CMakePrinter::clear_exports(path p) const
{
    if (p.empty())
        p = directories.storage_dir_obj;

    // projects
    for (auto &fp : boost::make_iterator_range(fs::directory_iterator(p), {}))
    {
        if (!fs::is_directory(fp))
            continue;

        // versions
        for (auto &fv : boost::make_iterator_range(fs::directory_iterator(fp), {}))
        {
            if (!fs::is_directory(fv) || !fs::exists(fv / "build"))
                continue;

            if (!fs::is_directory(fv) || !fs::exists(fv / "build"))
                continue;

            // configs
            for (auto &fc : boost::make_iterator_range(fs::directory_iterator(fv / "build"), {}))
            {
                if (!fs::is_directory(fc))
                    continue;

                boost::system::error_code ec;
                fs::remove_all(fc / "exports", ec);
            }
        }
    }
}

void CMakePrinter::print()
{
    print_configs();
}

void CMakePrinter::print_meta()
{
    print_meta_config_file(fs::current_path() / CPPAN_LOCAL_DIR / cmake_config_filename);
    print_include_guards_file(fs::current_path() / CPPAN_LOCAL_DIR / include_guard_filename);
    print_helper_file(fs::current_path() / CPPAN_LOCAL_DIR / cmake_helpers_filename);

    // print inserted files
    access_table->write_if_older(fs::current_path() / CPPAN_LOCAL_DIR / cmake_functions_filename, cmake_functions);
    access_table->write_if_older(fs::current_path() / CPPAN_LOCAL_DIR / CPP_HEADER_FILENAME, cppan_h);
}

void CMakePrinter::print_configs()
{
    auto src_dir = d.getDirSrc();
    fs::create_directories(src_dir);

    print_package_config_file(src_dir / cmake_config_filename);
    print_package_actions_file(src_dir / actions_filename);
    print_package_include_file(src_dir / include_guard_filename);

    if (d.flags[pfHeaderOnly] || cc->local_settings.local_build)
        return;

    auto obj_dir = d.getDirObj();
    fs::create_directories(obj_dir);

    // print object config files for non-local building
    print_object_config_file(obj_dir / cmake_config_filename);
    print_object_include_config_file(obj_dir / cmake_object_config_filename);
    print_object_export_file(obj_dir / exports_filename);
    print_object_build_file(obj_dir / non_local_build_file);
}

void CMakePrinter::print_bs_insertion(Context &ctx, const Project &p, const String &name, const String BuildSystemConfigInsertions::*i) const
{
    config_section_title(ctx, name);
    if (cc->getProjects().size() > 1)
    {
        ctx.addLine(cc->bs_insertions.*i);
        ctx.emptyLines(1);
    }
    ctx.addLine(p.bs_insertions.*i);
    ctx.emptyLines(1);

    for (auto &ol : p.options)
    {
        auto &s = ol.second.bs_insertions.*i;
        if (s.empty())
            continue;

        if (ol.first == "any")
        {
            ctx.addLine(s);
        }
        else
        {
            ctx.addLine("if (LIBRARY_TYPE STREQUAL \"" + boost::algorithm::to_upper_copy(ol.first) + "\")");
            ctx.increaseIndent();
            ctx.addLine(s);
            ctx.decreaseIndent();
            ctx.addLine("endif()");
            ctx.emptyLines(1);
        }
    }

    ctx.emptyLines(1);
}

void CMakePrinter::print_package_config_file(const path &fn) const
{
    if (!access_table->must_update_contents(fn))
        return;

    bool header_only = d.flags[pfHeaderOnly];
    const auto &p = cc->getProject(d.ppath.toString());
    const auto &dd = rd[d].dependencies;

    Context ctx;
    file_title(ctx, d);

    // deps
    print_dependencies(ctx, rd[d].dependencies, rc->local_settings.local_build);

    // settings
    {
        config_section_title(ctx, "settings");
        ctx.addLine("set(PACKAGE_NAME " + d.ppath.toString() + ")");
        ctx.addLine("set(PACKAGE_VERSION " + d.version.toString() + ")");
        ctx.addLine();
        ctx.addLine("set(LIBRARY_TYPE STATIC)");
        ctx.addLine();
        ctx.addLine("if (CPPAN_BUILD_SHARED_LIBS)");
        ctx.increaseIndent();
        ctx.addLine("set(LIBRARY_TYPE SHARED)");
        ctx.decreaseIndent();
        ctx.addLine("endif()");
        ctx.addLine();
        ctx.addLine("if (LIBRARY_TYPE_" + d.variable_name + ")");
        ctx.increaseIndent();
        ctx.addLine("set(LIBRARY_TYPE ${LIBRARY_TYPE_" + d.variable_name + "})");
        ctx.decreaseIndent();
        ctx.addLine("endif()");

        if (p.static_only)
            ctx.addLine("set(LIBRARY_TYPE STATIC)");
        else if (p.shared_only)
            ctx.addLine("set(LIBRARY_TYPE SHARED)");

        ctx.emptyLines(1);
    }

    print_bs_insertion(ctx, p, "pre sources", &BuildSystemConfigInsertions::pre_sources);

    // sources (also used for headers)
    config_section_title(ctx, "sources");
    if (p.build_files.empty())
        ctx.addLine("file(GLOB_RECURSE src \"*\")");
    else
    {
        ctx.addLine("set(src");
        ctx.increaseIndent();
        for (auto &f : p.build_files)
        {
            auto s = f;
            std::replace(s.begin(), s.end(), '\\', '/');
            ctx.addLine("${CMAKE_CURRENT_SOURCE_DIR}/" + s);
        }
        ctx.decreaseIndent();
        ctx.addLine(")");
    }
    ctx.addLine();

    // exclude files
    if (!p.exclude_from_build.empty())
    {
        config_section_title(ctx, "exclude files");
        for (auto &f : p.exclude_from_build)
            ctx << "list(REMOVE_ITEM src \"${CMAKE_CURRENT_SOURCE_DIR}/" << f.string() << "\")" << Context::eol;
        ctx.emptyLines(1);
    }

    print_bs_insertion(ctx, p, "post sources", &BuildSystemConfigInsertions::post_sources);

    for (auto &ol : p.options)
        for (auto &ll : ol.second.link_directories)
            ctx.addLine("link_directories(" + ll + ")");
    ctx.emptyLines(1);

    // target
    config_section_title(ctx, "target: " + d.target_name);
    if (d.flags[pfExecutable])
    {
        ctx << "add_executable                (" << d.target_name << " ${src})" << Context::eol;
    }
    else
    {
        if (header_only)
            ctx << "add_library                   (" << d.target_name << " INTERFACE)" << Context::eol;
        else
            ctx << "add_library                   (" << d.target_name << " ${LIBRARY_TYPE} ${src})" << Context::eol;
    }
    ctx.addLine();

    // local aliases
    ctx.addLine("set(target " + d.target_name + ")");
    ctx.addLine("set(this " + d.target_name + ")");
    ctx.addLine();

    // include directories
    {
        std::vector<Package> include_deps;
        for (auto &d : dd)
        {
            if (d.second.flags[pfIncludeDirectories])
                include_deps.push_back(d.second);
        }
        if (!p.include_directories.empty() || !include_deps.empty())
        {
            auto get_i_dir = [](const String &i)
            {
                return i;
                if (i.find("${CMAKE_CURRENT_SOURCE_DIR}") != i.npos ||
                    i.find("${CMAKE_CURRENT_BINARY_DIR}") != i.npos)
                    return i;
                return "${CMAKE_CURRENT_SOURCE_DIR}/" + i;
            };

            ctx << "target_include_directories    (" << d.target_name << Context::eol;
            ctx.increaseIndent();
            if (header_only)
            {
                for (auto &idir : p.include_directories.public_)
                    ctx.addLine("INTERFACE " + get_i_dir(idir.string()));
                for (auto &pkg : include_deps)
                {
                    auto &proj = cc->getProject(pkg.ppath.toString());
                    for (auto &i : proj.include_directories.public_)
                    {
                        auto ipath = pkg.getDirSrc() / i;
                        boost::system::error_code ec;
                        if (fs::exists(ipath, ec))
                            ctx.addLine("INTERFACE " + normalize_path(ipath));
                    }
                }
            }
            else
            {
                for (auto &idir : p.include_directories.public_)
                    ctx.addLine("PUBLIC " + get_i_dir(idir.string()));
                for (auto &idir : p.include_directories.private_)
                    ctx.addLine("PRIVATE " + get_i_dir(idir.string()));
                for (auto &pkg : include_deps)
                {
                    auto &proj = cc->getProject(pkg.ppath.toString());
                    for (auto &i : proj.include_directories.public_)
                    {
                        auto ipath = pkg.getDirSrc() / i;
                        boost::system::error_code ec;
                        if (fs::exists(ipath, ec))
                            ctx.addLine("PUBLIC " + normalize_path(ipath));
                    }
                    // no privates here
                }
            }
            ctx.decreaseIndent();
            ctx.addLine(")");
        }
    }

    // deps (direct)
    ctx.addLine("target_link_libraries         (" + d.target_name);
    ctx.increaseIndent();
    ctx.addLine((!header_only ? "PUBLIC" : "INTERFACE") + String(" ") + cppan_helpers_target);
    if (!header_only)
        ctx.addLine("PRIVATE" + String(" ") + cppan_helpers_private_target);
    for (auto &d1 : dd)
    {
        if (d1.second.flags[pfExecutable] ||
            d1.second.flags[pfIncludeDirectories])
            continue;
        if (header_only)
            ctx.addLine("INTERFACE " + d1.second.target_name);
        else
        {
            if (d1.second.flags[pfPrivateDependency])
                ctx.addLine("PRIVATE " + d1.second.target_name);
            else
                ctx.addLine("PUBLIC " + d1.second.target_name);
        }
    }
    ctx.decreaseIndent();
    ctx.addLine(")");
    ctx.addLine();
    ctx.addLine("if (NOT CPPAN_LOCAL_BUILD AND CMAKE_GENERATOR STREQUAL Ninja)");
    ctx.addLine("target_link_libraries         (" + d.target_name + " PRIVATE cppan-dummy)");
    ctx.addLine("endif()");
    ctx.addLine();

    // solution folder
    if (!header_only)
    {
        ctx << "set_target_properties         (" << d.target_name << " PROPERTIES" << Context::eol;
        ctx << "    FOLDER \"" + packages_folder + "/" << d.ppath.toString() << "/" << d.version.toString() << "\"" << Context::eol;
        ctx << ")" << Context::eol;
        ctx.emptyLines(1);
    }

    // options (defs etc.)
    {
        if (!header_only)
        {
            // pkg
            ctx.addLine("target_compile_definitions    (" + d.target_name);
            ctx.increaseIndent();
            ctx.addLine("PRIVATE   PACKAGE=\"" + d.ppath.toString() + "\"");
            ctx.addLine("PRIVATE   PACKAGE_NAME=\"" + d.ppath.toString() + "\"");
            ctx.addLine("PRIVATE   PACKAGE_VERSION=\"" + d.version.toString() + "\"");
            ctx.addLine("PRIVATE   PACKAGE_STRING=\"" + d.target_name + "\"");
            ctx.decreaseIndent();
            ctx.addLine(")");
        }

        // export/import
        ctx.addLine("if (LIBRARY_TYPE STREQUAL \"SHARED\")");
        ctx.increaseIndent();
        ctx.addLine("target_compile_definitions    (" + d.target_name);
        ctx.increaseIndent();
        if (!header_only)
        {
            ctx.addLine("PRIVATE   " CPPAN_EXPORT_PREFIX + d.variable_name + (d.flags[pfExecutable] ? "" : "=CPPAN_SYMBOL_EXPORT"));
            ctx.addLine("INTERFACE " CPPAN_EXPORT_PREFIX + d.variable_name + (d.flags[pfExecutable] ? "" : "=CPPAN_SYMBOL_IMPORT"));
        }
        else
            ctx.addLine("INTERFACE " CPPAN_EXPORT_PREFIX + d.variable_name + (d.flags[pfExecutable] ? "" : "="));
        ctx.decreaseIndent();
        ctx.addLine(")");
        ctx.decreaseIndent();
        ctx.addLine("else()");
        ctx.increaseIndent();
        ctx.addLine("target_compile_definitions    (" + d.target_name);
        ctx.increaseIndent();
        if (!header_only)
            ctx.addLine("PUBLIC    " CPPAN_EXPORT_PREFIX + d.variable_name + "=");
        else
            ctx.addLine("INTERFACE    " CPPAN_EXPORT_PREFIX + d.variable_name + "=");
        ctx.decreaseIndent();
        ctx.addLine(")");
        ctx.decreaseIndent();
        ctx.addLine("endif()");
        ctx.addLine();

        if (!d.flags[pfExecutable] && !header_only)
        {
            ctx.addLine(R"(set_target_properties(${this} PROPERTIES
    INSTALL_RPATH .
    BUILD_WITH_INSTALL_RPATH True
))");
        }
        ctx.addLine();

        /*if (d.flags[pfExecutable])
        {
        ctx.addLine("if (MSVC OR XCODE)");
        ctx.increaseIndent();
        for (auto &c : {"Debug","MinSizeRel","RelWithDebInfo"})
        {
        ctx.addLine("add_custom_command(TARGET ${this} POST_BUILD");
        ctx.increaseIndent();
        ctx.addLine("COMMAND ${CMAKE_COMMAND} -E copy_if_different");
        ctx.increaseIndent();
        ctx.addLine("$<TARGET_FILE:${this}>");
        ctx.addLine("$<TARGET_FILE_DIR:${this}>/../" + String(c) + "/$<TARGET_FILE_NAME:${this}>");
        ctx.decreaseIndent();
        ctx.decreaseIndent();
        ctx.addLine(")");
        ctx.addLine();
        }
        ctx.decreaseIndent();
        ctx.addLine("endif()");
        ctx.addLine();
        }*/

        for (auto &ol : p.options)
        {
            ctx.emptyLines(1);

            auto print_defs = [header_only, &ctx, this](const auto &ol)
            {
                if (ol.second.definitions.empty())
                    return;
                ctx << "target_compile_definitions    (" << d.target_name << Context::eol;
                ctx.increaseIndent();
                for (auto &def : ol.second.definitions)
                {
                    if (header_only)
                        ctx << "INTERFACE " << def.second << Context::eol;
                    else
                        ctx << boost::algorithm::to_upper_copy(def.first) << " " << def.second << Context::eol;
                }
                ctx.decreaseIndent();
                ctx.addLine(")");
            };
            auto print_set = [header_only, &ctx, this](const auto &a, const auto &s)
            {
                if (a.empty())
                    return;
                ctx << s << "(" << d.target_name << Context::eol;
                ctx.increaseIndent();
                for (auto &def : a)
                {
                    if (header_only)
                        ctx << "INTERFACE ";
                    else
                        ctx << "PUBLIC ";
                    ctx << def << Context::eol;
                }
                ctx.decreaseIndent();
                ctx.addLine(")");
                ctx.addLine();
            };
            auto print_options = [&ol, &print_defs, &print_set]
            {
                print_defs(ol);
                print_set(ol.second.include_directories, "target_include_directories");
                print_set(ol.second.link_libraries, "target_link_libraries");
            };

            if (ol.first == "any")
            {
                print_options();
            }
            else
            {
                ctx.addLine("if (LIBRARY_TYPE STREQUAL \"" + boost::algorithm::to_upper_copy(ol.first) + "\")");
                print_options();
                ctx.addLine("endif()");
            }
        }
        ctx.emptyLines(1);
    }

    print_bs_insertion(ctx, p, "post target", &BuildSystemConfigInsertions::post_target);

    // aliases
    if (!d.version.isBranch())
    {
        String tt = d.flags[pfExecutable] ? "add_executable" : "add_library";

        config_section_title(ctx, "aliases");

        {
            Version ver = d.version;
            ver.patch = -1;
            ctx << tt << "(" << d.ppath.toString() + "-" + ver.toAnyVersion() << " ALIAS " << d.target_name << ")" << Context::eol;
            ver.minor = -1;
            ctx << tt << "(" << d.ppath.toString() + "-" + ver.toAnyVersion() << " ALIAS " << d.target_name << ")" << Context::eol;
            ctx << tt << "(" << d.ppath.toString() << " ALIAS " << d.target_name << ")" << Context::eol;
            ctx.addLine();
        }

        {
            Version ver = d.version;
            ctx << tt << "(" << d.ppath.toString("::") + "-" + ver.toAnyVersion() << " ALIAS " << d.target_name << ")" << Context::eol;
            ver.patch = -1;
            ctx << tt << "(" << d.ppath.toString("::") + "-" + ver.toAnyVersion() << " ALIAS " << d.target_name << ")" << Context::eol;
            ver.minor = -1;
            ctx << tt << "(" << d.ppath.toString("::") + "-" + ver.toAnyVersion() << " ALIAS " << d.target_name << ")" << Context::eol;
            ctx << tt << "(" << d.ppath.toString("::") << " ALIAS " << d.target_name << ")" << Context::eol;
            ctx.addLine();
        }

        if (!p.aliases.empty())
        {
            ctx.addLine("# user-defined");
            for (auto &a : p.aliases)
                ctx << tt << "(" << a << " ALIAS " << d.target_name << ")" << Context::eol;
            ctx.addLine();
        }
    }

    // export
    config_section_title(ctx, "export");
    ctx.addLine("export(TARGETS " + d.target_name + " FILE " + exports_dir + d.variable_name + ".cmake)");

    ctx.emptyLines(1);

    print_bs_insertion(ctx, p, "post alias", &BuildSystemConfigInsertions::post_alias);

    // dummy target for IDEs with headers only
    if (header_only)
    {
        config_section_title(ctx, "IDE dummy target for headers");

        auto tgt = d.target_name + "-headers";
        ctx.addLine("if (CPPAN_SHOW_IDE_PROJECTS)");
        ctx.addLine("add_custom_target(" + tgt + " SOURCES ${src})");
        ctx.addLine();
        ctx << "set_target_properties         (" << tgt << " PROPERTIES" << Context::eol;
        ctx << "    FOLDER \"" + packages_folder + "/" << d.ppath.toString() << "/" << d.version.toString() << "\"" << Context::eol;
        ctx << ")" << Context::eol;
        ctx.addLine("endif()");
        ctx.emptyLines(1);
    }

    // source groups
    print_source_groups(ctx, fn.parent_path());

    // eof
    ctx.addLine(config_delimeter);
    ctx.addLine();
    ctx.splitLines();

    access_table->write_if_older(fn, ctx.getText());
}

void CMakePrinter::print_package_actions_file(const path &fn) const
{
    if (!access_table->must_update_contents(fn))
        return;

    const auto &p = cc->getProject(d.ppath.toString());
    Context ctx;
    file_title(ctx, d);
    ctx.addLine(config_delimeter);
    ctx.addLine();
    ctx.addLine("set(CMAKE_CURRENT_SOURCE_DIR_OLD ${CMAKE_CURRENT_SOURCE_DIR})");
    ctx.addLine("set(CMAKE_CURRENT_SOURCE_DIR \"" + normalize_path(fn.parent_path().string()) + "\")");
    print_bs_insertion(ctx, p, "pre sources", &BuildSystemConfigInsertions::pre_sources);
    ctx.addLine("file(GLOB_RECURSE src \"*\")");
    print_bs_insertion(ctx, p, "post sources", &BuildSystemConfigInsertions::post_sources);
    print_bs_insertion(ctx, p, "post target", &BuildSystemConfigInsertions::post_target);
    print_bs_insertion(ctx, p, "post alias", &BuildSystemConfigInsertions::post_alias);
    ctx.addLine("set(CMAKE_CURRENT_SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR_OLD})");
    ctx.addLine();
    ctx.addLine(config_delimeter);
    ctx.addLine();
    access_table->write_if_older(fn, ctx.getText());
}

void CMakePrinter::print_package_include_file(const path &fn) const
{
    if (!access_table->must_update_contents(fn))
        return;

    Context ctx;
    String ig = INCLUDE_GUARD_PREFIX + d.variable_name;

    file_title(ctx, d);

    ctx.addLine("if (" + ig + ")");
    ctx.addLine("    return()");
    ctx.addLine("endif()");
    ctx.addLine();
    ctx.addLine("set(" + ig + " 1 CACHE BOOL \"\" FORCE)");
    ctx.addLine();
    ctx.addLine("add_subdirectory(\"" + normalize_path(fn.parent_path().string()) + "\" \"" + get_binary_path(d) + "\")");
    ctx.addLine();

    access_table->write_if_older(fn, ctx.getText());
}

void CMakePrinter::print_object_config_file(const path &fn) const
{
    if (!access_table->must_update_contents(fn))
        return;

    auto src_dir = d.getDirSrc();
    auto obj_dir = d.getDirObj();

    Context ctx;
    file_title(ctx, d);

    {
        config_section_title(ctx, "cmake settings");
        ctx.addLine(cmake_minimum_required);
        ctx.addLine();
        ctx.addLine("set(CMAKE_RUNTIME_OUTPUT_DIRECTORY " + normalize_path(directories.storage_dir_bin) + "/${OUTPUT_DIR})");
        ctx.addLine("set(CMAKE_LIBRARY_OUTPUT_DIRECTORY " + normalize_path(directories.storage_dir_lib) + "/${OUTPUT_DIR})");
        ctx.addLine("set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY " + normalize_path(directories.storage_dir_lib) + "/${OUTPUT_DIR})");
        ctx.addLine();

        /*if (d.flags[pfExecutable])
        {
        ctx.addLine("if (MSVC OR XCODE)");
        for (auto &c : cmake_configuration_types_no_rel)
        ctx.addLine("set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_" + c + " ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/Release)");
        ctx.addLine("endif()");
        ctx.addLine();
        }*/
    }

    config_section_title(ctx, "project settings");
    ctx.addLine("project(" + d.variable_name + " C CXX)");
    ctx.addLine();

    config_section_title(ctx, "compiler & linker settings");
    ctx.addLine(R"(if (NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

if (MSVC)
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} /MP")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} /MP")

    string(FIND "${OUTPUT_DIR}" "-mt" mt)
    if (NOT mt EQUAL -1)
        set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS} /MT")
        set(CMAKE_CXX_FLAGS_RELWITHDEBINFO "${CMAKE_CXX_FLAGS} /MT")
        set(CMAKE_CXX_FLAGS_MINSIZEREL "${CMAKE_CXX_FLAGS} /MT")
        set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS} /MTd")
    endif()

    if (0)# OR CMAKE_GENERATOR STREQUAL Ninja)
        string(TOLOWER "${CMAKE_CXX_COMPILER}" inc)
        string(REGEX MATCH ".*/vc/bin" inc "${inc}")

        include_directories(BEFORE SYSTEM ${inc}/include)
        set(ENV{INCLUDE} ${inc}/include)

        set(lib)
        if (CMAKE_SYSTEM_PROCESSOR STREQUAL amd64)
            set(lib /${CMAKE_SYSTEM_PROCESSOR})
        endif()
        link_directories(${inc}/lib${lib})
        set(ENV{LIB} ${inc}/lib${lib})
    endif()
endif()
)");

    // recursive calls
    {
        config_section_title(ctx, "cppan setup");

        ctx.addLine("add_subdirectory(cppan)");
        fs::copy_file(src_dir / CPPAN_FILENAME, obj_dir / CPPAN_FILENAME, fs::copy_option::overwrite_if_exists);

        if (pc->internal_options.invocations.find(d) != pc->internal_options.invocations.end())
            throw std::runtime_error("Circular dependency detected. Project: " + d.target_name);

        silent = true;
        auto old_dir = fs::current_path();
        fs::current_path(obj_dir);

        Config c(obj_dir);
        c.pkg = d;
        c.internal_options.current_package = d;
        c.internal_options.invocations = pc->internal_options.invocations;
        c.internal_options.invocations.insert(d);
        c.disable_run_cppan_target = true;
        c.process();

        fs::current_path(old_dir);
        if (pc->internal_options.current_package.empty())
            silent = false;
    }

    // main include
    {
        config_section_title(ctx, "main include");
        auto mi = src_dir;
        add_subdirectory(ctx, mi.string(), get_binary_path(d));
        ctx.emptyLines(1);
        auto ig = INCLUDE_GUARD_PREFIX + d.variable_name;
        // do not remove this as it turns off rebuilds
        ctx.addLine("set(" + ig + " 0 CACHE BOOL \"\" FORCE)");
        ctx.emptyLines(1);
    }

    // eof
    ctx.addLine(config_delimeter);
    ctx.addLine();
    ctx.splitLines();

    access_table->write_if_older(fn, ctx.getText());
}

void CMakePrinter::print_object_include_config_file(const path &fn) const
{
    if (!access_table->must_update_contents(fn))
        return;

    const auto &p = cc->getProject(d.ppath.toString());
    const auto &dd = rd[d].dependencies;

    Context ctx;
    file_title(ctx, d);

    ctx.addLine("set(target " + d.target_name + ")");
    ctx.addLine();
    if (!p.aliases.empty())
    {
        ctx.addLine("set(aliases");
        ctx.increaseIndent();
        for (auto &a : p.aliases)
            ctx.addLine(a);
        ctx.decreaseIndent();
        ctx.addLine(")");
        ctx.addLine();
    }
    ctx.addLine("set(current_dir " + normalize_path(fn.parent_path()) + ")");
    if (!d.flags[pfExecutable])
        ctx.addLine("get_configuration(config)");
    else
        ctx.addLine("get_configuration_exe(config)");
    ctx.addLine("set(build_dir ${current_dir}/build/${config})");
    ctx.addLine("set(export_dir ${build_dir}/exports)");
    ctx.addLine("set(import ${export_dir}/" + d.variable_name + ".cmake)");
    ctx.addLine("set(import_fixed ${export_dir}/" + d.variable_name + "-fixed.cmake)");
    ctx.addLine("set(aliases_file ${export_dir}/" + d.variable_name + "-aliases.cmake)");
    ctx.addLine();
    ctx.addLine(R"(if (NOT EXISTS ${import} OR NOT EXISTS ${import_fixed})
    set(lock ${build_dir}/generate.lock)

    file(LOCK ${lock} TIMEOUT 0 RESULT_VARIABLE lock_result)
    if (NOT ${lock_result} EQUAL 0)
        message(STATUS "WARNING: Target: ${target}")
        message(STATUS "WARNING: Other project is being bootstrapped right now or you hit a circular deadlock.")
        message(STATUS "WARNING: If you aren't building other projects right now feel free to kill this process or it will be stopped in 90 seconds.")

        file(LOCK ${lock} TIMEOUT 90 RESULT_VARIABLE lock_result)

        if (NOT ${lock_result} EQUAL 0)
            message(FATAL_ERROR "Lock error: ${lock_result}")
        endif()
    endif()

    # double check
    if (NOT EXISTS ${import} OR NOT EXISTS ${import_fixed})
        message(STATUS "")
        message(STATUS "Preparing build tree for ${target} with config ${config}")
        message(STATUS "")

        #find_program(ninja ninja)
        #set(generator Ninja)
        set(generator ${CMAKE_GENERATOR})
        if (MSVC
            OR "${ninja}" STREQUAL "ninja-NOTFOUND"
            OR CYGWIN # for me it's not working atm
        )
            set(generator ${CMAKE_GENERATOR})
        endif()
)");
    if (d.flags[pfExecutable])
    {
        ctx.addLine(R"(
            execute_process(
                COMMAND ${CMAKE_COMMAND}
                    -H${current_dir} -B${build_dir}
                    #-DCMAKE_C_COMPILER=${CMAKE_C_COMPILER}
                    #-DCMAKE_CXX_COMPILER=${CMAKE_CXX_COMPILER}
                    #-G "${generator}"
                    -DOUTPUT_DIR=${config}
                    -DCPPAN_BUILD_SHARED_LIBS=0 # TODO: try to work 0->1
            )
)");
    }
    else
    {
        ctx.addLine(R"(
        if (CMAKE_TOOLCHAIN_FILE)
            execute_process(
                COMMAND ${CMAKE_COMMAND}
                    -H${current_dir} -B${build_dir}
                    -DCMAKE_TOOLCHAIN_FILE=${CMAKE_TOOLCHAIN_FILE}
                    -DCMAKE_MAKE_PROGRAM=${CMAKE_MAKE_PROGRAM}
                    -G "${generator}"
                    -DOUTPUT_DIR=${config}
                    -DCPPAN_BUILD_SHARED_LIBS=${CPPAN_BUILD_SHARED_LIBS}
            )
        else()
            execute_process(
                COMMAND ${CMAKE_COMMAND}
                    -H${current_dir} -B${build_dir}
                    -DCMAKE_C_COMPILER=${CMAKE_C_COMPILER}
                    -DCMAKE_CXX_COMPILER=${CMAKE_CXX_COMPILER}
                    -G "${generator}"
                    -DOUTPUT_DIR=${config}
                    -DCPPAN_BUILD_SHARED_LIBS=${CPPAN_BUILD_SHARED_LIBS}
            )
        endif()
)");
    }
    ctx.addLine(R"(
        file(WRITE ${aliases_file} "${aliases}")
        execute_process(
            COMMAND cppan internal-fix-imports ${target} ${aliases_file} ${import} ${import_fixed}
        )
    endif()

    file(LOCK ${lock} RELEASE)
endif()
)");

    ctx.addLine("if (NOT TARGET " + d.target_name + ")");
    ctx.addLine("     include(${import_fixed})");
    ctx.addLine("endif()");
    ctx.addLine();

    config_section_title(ctx, "import direct deps");
    ctx.addLine("include(${current_dir}/exports.cmake)");
    ctx.addLine();

    // src target
    {
        auto target = d.target_name + "-sources";
        auto dir = d.getDirSrc();

        ctx.addLine("if (CPPAN_SHOW_IDE_PROJECTS)");
        ctx.addLine();
        config_section_title(ctx, "sources target (for IDE only)");
        ctx.addLine("if (NOT TARGET " + target + ")");
        ctx.increaseIndent();
        ctx.addLine("file(GLOB_RECURSE src \"" + normalize_path(dir) + "/*\")");
        ctx.addLine();
        ctx.addLine("add_custom_target(" + target);
        ctx.addLine("    SOURCES ${src}");
        ctx.addLine(")");
        ctx.addLine();

        // solution folder
        ctx << "set_target_properties         (" << target << " PROPERTIES" << Context::eol;
        ctx << "    FOLDER \"" + packages_folder + "/" << d.ppath.toString() << "/" << d.version.toString() << "\"" << Context::eol;
        ctx << ")" << Context::eol;
        ctx.decreaseIndent();
        ctx.addLine("endif()");
        ctx.emptyLines(1);

        // source groups
        print_source_groups(ctx, dir);
        ctx.addLine("endif(CPPAN_SHOW_IDE_PROJECTS)");
    }

    // eof
    ctx.emptyLines(1);
    ctx.addLine(config_delimeter);
    ctx.addLine();
    ctx.splitLines();

    access_table->write_if_older(fn, ctx.getText());
}

void CMakePrinter::print_object_export_file(const path &fn) const
{
    if (!access_table->must_update_contents(fn))
        return;

    const auto &dd = rd[d].dependencies;
    Context ctx;
    file_title(ctx, d);

    for (auto &dp : dd)
    {
        auto &dep = dp.second;

        if (dep.flags[pfIncludeDirectories])
            continue;

        auto b = dep.getDirObj();
        auto p = b / "build" / "${config}" / "exports" / (dep.variable_name + "-fixed.cmake");

        if (!dep.flags[pfHeaderOnly])
            ctx.addLine("include(\"" + normalize_path(b / exports_filename) + "\")");
        ctx.addLine("if (NOT TARGET " + dep.target_name + ")");
        ctx.increaseIndent();
        if (dep.flags[pfHeaderOnly])
            add_subdirectory(ctx, dep.getDirSrc().string());
        else
        {
            ctx.addLine("if (NOT EXISTS \"" + normalize_path(p) + "\")");
            ctx.addLine("    include(\"" + normalize_path(b / cmake_object_config_filename) + "\")");
            ctx.addLine("endif()");
            ctx.addLine("include(\"" + normalize_path(p) + "\")");
        }
        ctx.decreaseIndent();
        ctx.addLine("endif()");
        ctx.addLine();
    }

    access_table->write_if_older(fn, ctx.getText());
}

void CMakePrinter::print_object_build_file(const path &fn) const
{
    if (!access_table->must_update_contents(fn))
        return;

    const auto &dd = rd[d].dependencies;
    Context ctx;
    file_title(ctx, d);

    auto fn1 = normalize_path(directories.storage_dir_src / d.ppath.toString() / get_stamp_filename(d.version.toString()));
    ctx.addLine(R"(set(REBUILD 1)

set(fn1 ")" + fn1 + R"(")
set(fn2 "${BUILD_DIR}/)" + cppan_stamp_filename + R"(")

file(READ ${fn1} f1)
if (EXISTS ${fn2})
    file(READ ${fn2} f2)
    if (f1 STREQUAL f2)
        set(REBUILD 0)
    endif()
else()
    file(WRITE ${fn2} "${f1}")
endif()

if (NOT REBUILD AND EXISTS ${TARGET_FILE})
    return()
endif()

set(lock ${BUILD_DIR}/build.lock)

file(LOCK ${lock} RESULT_VARIABLE lock_result)
if (NOT ${lock_result} EQUAL 0)
    message(FATAL_ERROR "Lock error: ${lock_result}")
endif()

# double check
if (NOT REBUILD AND EXISTS ${TARGET_FILE})
    # release before exit
    file(LOCK ${lock} RELEASE)

    return()
endif()

execute_process(COMMAND ${CMAKE_COMMAND} -E copy ${fn1} ${fn2})

if (CONFIG)
)");
    if (d.flags[pfExecutable])
    {
        ctx.addLine(R"(
    execute_process(
        COMMAND ${CMAKE_COMMAND}
            --build ${BUILD_DIR}
            --config ${CONFIG}#Release # FIXME: always build exe with Release conf
    ))");
    }
    else
    {
        ctx.addLine(R"(
    execute_process(
        COMMAND ${CMAKE_COMMAND}
            --build ${BUILD_DIR}
            --config ${CONFIG}
    ))");
    }
    ctx.addLine(R"(
else()
    find_program(make make)
    if (${make} STREQUAL "make-NOTFOUND")
        execute_process(
            COMMAND ${CMAKE_COMMAND}
                --build ${BUILD_DIR}
        )
    else()
        get_number_of_cores(N)
        execute_process(
            COMMAND make -j${N} -C ${BUILD_DIR}
        )
    endif()
endif()

file(LOCK ${lock} RELEASE)
)");

    access_table->write_if_older(fn, ctx.getText());
}

void CMakePrinter::print_meta_config_file(const path &fn) const
{
    if (!access_table->must_update_contents(fn))
        return;

    Context ctx;
    ctx.addLine("#");
    ctx.addLine("# cppan");
    ctx.addLine("# meta config file");
    ctx.addLine("#");
    ctx.addLine();
    ctx.addLine(cmake_minimum_required);
    ctx.addLine();

    config_section_title(ctx, "variables");
    ctx.addLine("set(CPPAN_BUILD 1 CACHE STRING \"CPPAN is turned on\")");
    ctx.addLine();
    ctx.addLine("set(CPPAN_SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR})");
    ctx.addLine("set(CPPAN_BINARY_DIR ${CMAKE_CURRENT_BINARY_DIR})");
    ctx.addLine();
    ctx.addLine("set(CMAKE_POSITION_INDEPENDENT_CODE ON)");
    ctx.addLine();
    ctx.addLine("set(${CMAKE_CXX_COMPILER_ID} 1)");
    ctx.addLine();
    ctx.addLine(String("set(CPPAN_LOCAL_BUILD ") + (cc->local_settings.local_build ? "1" : "0") + ")");
    ctx.addLine(String("set(CPPAN_SHOW_IDE_PROJECTS ") + (cc->local_settings.show_ide_projects ? "1" : "0") + ")");
    ctx.addLine();

    ctx.addLine("include(" + cmake_helpers_filename + ")");
    // include guard before deps
    if (cc == rc)
        ctx.addLine("include(" + include_guard_filename + ")");
    ctx.addLine();

    // deps
    print_dependencies(ctx, rd[d].dependencies, cc->local_settings.local_build);

    // include guard after deps
    if (cc == rc)
        ctx.addLine("include(" + include_guard_filename + ")");

    // lib
    const String cppan_project_name = "cppan";
    config_section_title(ctx, "main library");
    ctx.addLine("add_library                   (" + cppan_project_name + " INTERFACE)");
    ctx.addLine("target_link_libraries         (" + cppan_project_name);
    ctx.increaseIndent();
    ctx.addLine("INTERFACE " + cppan_helpers_target);
    for (auto &p : rd[d].dependencies)
    {
        if (p.second.flags[pfExecutable] || p.second.flags[pfIncludeDirectories])
            continue;
        ctx.addLine("INTERFACE " + p.second.target_name);
    }
    ctx.decreaseIndent();
    ctx.addLine(")");
    ctx.addLine();
    ctx.addLine("export(TARGETS " + cppan_project_name + " FILE " + exports_dir + "cppan.cmake)");

    // exe deps
    if (!cc->local_settings.local_build)
    {
        config_section_title(ctx, "exe deps");

        auto dd = rd[d].dependencies;
        if (!cc->internal_options.current_package.empty())
            dd = rd[cc->internal_options.current_package].dependencies;

        for (auto &dp : dd)
        {
            auto &d = dp.second;
            if (d.flags[pfExecutable])
                ctx.addLine("add_dependencies(" + d.target_name + " " + cppan_project_name + ")");
        }
    }

    ctx.emptyLines(1);
    ctx.addLine(config_delimeter);
    ctx.addLine();

    access_table->write_if_older(fn, ctx.getText());
}

void CMakePrinter::print_include_guards_file(const path &fn) const
{
    if (!access_table->must_update_contents(fn))
        return;

    if (cc != rc)
        return;

    // turn off header guards
    Context ctx;
    file_title(ctx, d);
    for (auto &ig : include_guards)
        ctx.addLine("set(" + ig + " 0 CACHE BOOL \"\" FORCE)");
    access_table->write_if_older(fn, ctx.getText());
}

void CMakePrinter::print_helper_file(const path &fn) const
{
    if (!access_table->must_update_contents(fn))
        return;

    Context ctx;
    ctx.addLine("#");
    ctx.addLine("# cppan");
    ctx.addLine("# helper routines");
    ctx.addLine("#");
    ctx.addLine();

    config_section_title(ctx, "cmake setup");
    ctx.addLine(R"(# Use solution folders.
set_property(GLOBAL PROPERTY USE_FOLDERS ON))");
    ctx.addLine();

    config_section_title(ctx, "macros & functions");
    ctx.addLine("include(" + cmake_functions_filename + ")");

    config_section_title(ctx, "variables");
    ctx.addLine("get_configuration(config)");
    ctx.addLine();

    config_section_title(ctx, "export/import");
    ctx.addLine(R"str(if (MSVC)
    set(CPPAN_EXPORT "__declspec(dllexport)")
    set(CPPAN_IMPORT "__declspec(dllimport)")
endif()

if (MINGW)
    set(CPPAN_EXPORT "__attribute__((__dllexport__))")
    set(CPPAN_IMPORT "__attribute__((__dllimport__))")
elseif(GNU)
    set(CPPAN_EXPORT "__attribute__((__visibility__(\"default\")))")
    set(CPPAN_IMPORT)
endif()

if (SUN) # TODO: check it in real environment
    set(CPPAN_EXPORT "__global")
    set(CPPAN_IMPORT "__global")
endif())str");

    // cmake includes
    config_section_title(ctx, "cmake includes");
    ctx.addLine(R"(include(CheckCXXSymbolExists)
include(CheckFunctionExists)
include(CheckIncludeFiles)
include(CheckLibraryExists)
include(CheckTypeSize)
include(TestBigEndian))");
    ctx.addLine();

    config_section_title(ctx, "common checks");

    // read vars file
    ctx.addLine("set(vars_file \"" + normalize_path(directories.storage_dir_cfg) + "/${config}.cmake\")");
    ctx.addLine("read_variables_file(${vars_file})");
    ctx.addLine();

    ctx.addLine("if (NOT DEFINED WORDS_BIGENDIAN)");
    ctx.increaseIndent();
    ctx.addLine("test_big_endian(WORDS_BIGENDIAN)");
    ctx.addLine("add_variable(WORDS_BIGENDIAN)");
    ctx.decreaseIndent();
    ctx.addLine("endif()");
    // aliases
    ctx.addLine("set(BIG_ENDIAN ${WORDS_BIGENDIAN} CACHE STRING \"endianness alias\")");
    ctx.addLine("set(BIGENDIAN ${WORDS_BIGENDIAN} CACHE STRING \"endianness alias\")");
    ctx.addLine("set(HOST_BIG_ENDIAN ${WORDS_BIGENDIAN} CACHE STRING \"endianness alias\")");
    ctx.addLine();

    // checks
    config_section_title(ctx, "checks");

    auto convert_function = [](const auto &s)
    {
        return "HAVE_" + boost::algorithm::to_upper_copy(s);
    };
    auto convert_include = [](const auto &s)
    {
        auto v_def = "HAVE_" + boost::algorithm::to_upper_copy(s);
        for (auto &c : v_def)
        {
            if (!isalnum(c))
                c = '_';
        }
        return v_def;
    };
    auto convert_type = [](const auto &s, const std::string &prefix = "HAVE_")
    {
        String v_def = prefix;
        v_def += boost::algorithm::to_upper_copy(s);
        for (auto &c : v_def)
        {
            if (c == '*')
                c = 'P';
            else if (!isalnum(c))
                c = '_';
        }
        return v_def;
    };

    auto add_checks = [&ctx](const auto &a, const String &s, auto &&f)
    {
        for (auto &v : a)
        {
            auto val = f(v);
            ctx.addLine("if (NOT DEFINED " + val + ")");
            ctx.increaseIndent();
            ctx.addLine(s + "(\"" + v + "\" " + val + ")");
            ctx.addLine("add_variable(" + val + ")");
            ctx.decreaseIndent();
            ctx.addLine("endif()");
        }
        ctx.emptyLines(1);
    };
    auto add_symbol_checks = [&ctx](const auto &a, const String &s, auto &&f)
    {
        for (auto &v : a)
        {
            auto val = f(v.first);
            ctx.addLine("if (NOT DEFINED " + val + ")");
            ctx.increaseIndent();
            ctx << s + "(\"" + v.first + "\" \"";
            for (auto &h : v.second)
                ctx << h << ";";
            ctx << "\" " << val << ")" << Context::eol;
            ctx.addLine("add_variable(" + val + ")");
            ctx.decreaseIndent();
            ctx.addLine("endif()");
        }
        ctx.emptyLines(1);
    };
    auto add_if_definition = [&ctx](const String &s, auto &&... defs)
    {
        auto print_def = [&ctx](auto &&s)
        {
            ctx << "INTERFACE " << s << Context::eol;
            return 0;
        };
        ctx.addLine("if (" + s + ")");
        ctx.increaseIndent();
        ctx << "target_compile_definitions(" << cppan_helpers_target << Context::eol;
        ctx.increaseIndent();
        print_def(s);
        using expand_type = int[];
        expand_type{ 0, print_def(std::forward<decltype(defs)>(defs))... };
        ctx.decreaseIndent();
        ctx.addLine(")");
        ctx.decreaseIndent();
        ctx.addLine("endif()");
        ctx.addLine();
    };
    auto add_check_definitions = [&ctx, &add_if_definition](const auto &a, auto &&f)
    {
        for (auto &v : a)
            add_if_definition(f(v));
    };
    auto add_check_symbol_definitions = [&ctx, &add_if_definition](const auto &a, auto &&f)
    {
        for (auto &v : a)
            add_if_definition(f(v.first));
    };

    add_checks(cc->check_functions, "check_function_exists", convert_function);
    add_symbol_checks(cc->check_symbols, "check_cxx_symbol_exists", convert_function);
    add_checks(cc->check_includes, "check_include_files", convert_include);
    add_checks(cc->check_types, "check_type_size", convert_type);

    for (auto &v : cc->check_types)
    {
        ctx.addLine("if (" + convert_type(v) + ")");
        ctx.increaseIndent();
        ctx.addLine("set(" + convert_type(v, "SIZE_OF_") + " ${" + convert_type(v) + "} CACHE STRING \"\")");
        ctx.addLine("set(" + convert_type(v, "SIZEOF_") + " ${" + convert_type(v) + "} CACHE STRING \"\")");
        ctx.decreaseIndent();
        ctx.addLine("endif()");
        ctx.addLine();
    }

    // write vars file
    ctx.addLine("if (CPPAN_NEW_VARIABLE_ADDED)");
    ctx.addLine("    write_variables_file(${vars_file})");
    ctx.addLine("endif()");

    // fixups
    // put bug workarounds here
    //config_section_title(ctx, "fixups");
    ctx.emptyLines(1);

    // dummy (compiled?) target
    {
        config_section_title(ctx, "dummy compiled target");
        ctx.addLine("# this target will be always built before any other");
        ctx.addLine("if (CMAKE_GENERATOR STREQUAL Ninja)");
        ctx.addLine("    set(f ${CMAKE_CURRENT_BINARY_DIR}/cppan_dummy.cpp)");
        ctx.addLine("    file_write_once(${f} \"void __cppan_dummy() {}\")");
        ctx.addLine("    add_library(" + cppan_dummy_target + " ${f})");
        //ctx.addLine("    export(TARGETS " + cppan_dummy_target + " FILE " + exports_dir + cppan_dummy_target + ".cmake)");
        ctx.addLine("elseif(MSVC)");
        ctx.addLine("    add_custom_target(" + cppan_dummy_target + " ALL DEPENDS cppan_intentionally_missing_file.txt)");
        ctx.addLine("else()");
        ctx.addLine("    add_custom_target(" + cppan_dummy_target + " ALL)");
        ctx.addLine("endif()");
        ctx.addLine();
        ctx.addLine("set_target_properties(" + cppan_dummy_target + " PROPERTIES\n    FOLDER \"cppan/service\"\n)");
        ctx.emptyLines(1);
    }

    // public library
    {
        config_section_title(ctx, "helper interface library");

        ctx.addLine("add_library(" + cppan_helpers_target + " INTERFACE)");
        ctx.addLine("add_dependencies(" + cppan_helpers_target + " " + cppan_dummy_target + ")");
        ctx.addLine();

        // common include directories
        ctx.addLine("target_include_directories(" + cppan_helpers_target);
        ctx.increaseIndent();
        ctx.addLine("INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}");
        ctx.decreaseIndent();
        ctx.addLine(")");
        ctx.addLine();

        // common definitions
        ctx.addLine("target_compile_definitions(" + cppan_helpers_target);
        ctx.increaseIndent();
        ctx.addLine("INTERFACE CPPAN"); // build is performed under CPPAN
        ctx.addLine("INTERFACE CPPAN_BUILD"); // build is performed under CPPAN
        ctx.addLine("INTERFACE CPPAN_CONFIG=\"${config}\"");
        ctx.addLine("INTERFACE CPPAN_SYMBOL_EXPORT=${CPPAN_EXPORT}");
        ctx.addLine("INTERFACE CPPAN_SYMBOL_IMPORT=${CPPAN_IMPORT}");
        ctx.decreaseIndent();
        ctx.addLine(")");
        ctx.addLine();

        // common link libraries
        ctx.addLine(R"(if (WIN32)
target_link_libraries()" + cppan_helpers_target + R"(
    INTERFACE Ws2_32
)
else()
    find_library(pthread pthread)
    if (NOT ${pthread} STREQUAL "pthread-NOTFOUND")
        target_link_libraries()" + cppan_helpers_target + R"(
            INTERFACE pthread
        )
    endif()
    find_library(rt rt)
    if (NOT ${rt} STREQUAL "rt-NOTFOUND")
        target_link_libraries()" + cppan_helpers_target + R"(
            INTERFACE rt
        )
    endif()
endif()
)");
        ctx.addLine();

        // Do not use APPEND here. It's the first file that will clear cppan.cmake.
        ctx.addLine("export(TARGETS " + cppan_helpers_target + " FILE " + exports_dir + cppan_helpers_target + ".cmake)");
        ctx.emptyLines(1);
    }

    // private library
    {
        config_section_title(ctx, "private helper interface library");

        ctx.addLine("add_library(" + cppan_helpers_private_target + " INTERFACE)");
        ctx.addLine("add_dependencies(" + cppan_helpers_private_target + " " + cppan_dummy_target + ")");
        ctx.addLine();

        // msvc
        ctx.addLine(R"(if (MSVC)
target_compile_definitions()" + cppan_helpers_private_target + R"(
    INTERFACE _CRT_SECURE_NO_WARNINGS # disable warning about non-standard functions
)
target_compile_options()" + cppan_helpers_private_target + R"(
    INTERFACE /wd4005 # macro redefinition
    INTERFACE /wd4996 # The POSIX name for this item is deprecated.
)
endif()
)");

        // Do not use APPEND here. It's the first file that will clear cppan.cmake.
        ctx.addLine("export(TARGETS " + cppan_helpers_private_target + " FILE " + exports_dir + cppan_helpers_private_target + ".cmake)");
        ctx.emptyLines(1);
    }

    // global definitions
    config_section_title(ctx, "global definitions");

    Context local;
    bool has_defs = false;
    local << "target_compile_definitions(" << cppan_helpers_target << Context::eol;
    local.increaseIndent();
    for (auto &o : cc->global_options)
    {
        for (auto &opt : o.second.global_definitions)
        {
            local.addLine("INTERFACE " + opt);
            has_defs = true;
        }
    }
    local.decreaseIndent();
    local.addLine(")");
    local.addLine();
    if (has_defs)
        ctx += local;

    // definitions
    config_section_title(ctx, "definitions");

    add_if_definition("WORDS_BIGENDIAN", "BIGENDIAN", "BIG_ENDIAN", "HOST_BIG_ENDIAN");

    add_check_definitions(cc->check_functions, convert_function);
    add_check_symbol_definitions(cc->check_symbols, convert_function);
    add_check_definitions(cc->check_includes, convert_include);
    add_check_definitions(cc->check_types, convert_type);

    if (cc->local_settings.add_run_cppan_target && !cc->disable_run_cppan_target)
    {
        // re-run cppan when root cppan.yml is changed
        config_section_title(ctx, "cppan regenerator");
        ctx.addLine(R"(set(file ${CMAKE_CURRENT_BINARY_DIR}/run-cppan.txt)
add_custom_command(OUTPUT ${file}
    COMMAND cppan -d ${PROJECT_SOURCE_DIR}
    COMMAND ${CMAKE_COMMAND} -E echo "" > ${file}
    DEPENDS ${PROJECT_SOURCE_DIR}/cppan.yml
)
add_custom_target(run-cppan
    DEPENDS ${file}
    SOURCES
        ${PROJECT_SOURCE_DIR}/cppan.yml
        ${PROJECT_SOURCE_DIR}/cppan/)" + cmake_functions_filename + R"(
        ${PROJECT_SOURCE_DIR}/cppan/)" + cmake_helpers_filename + R"(
)
add_dependencies()" + cppan_helpers_target + R"( run-cppan)
set_target_properties(run-cppan PROPERTIES
    FOLDER "cppan/service"
))");
    }

    // direct deps' build actions for non local build
    if (!cc->local_settings.local_build)
    {
        config_section_title(ctx, "custom actions for dummy target");

        Packages bdeps;
        gather_build_deps(ctx, rd[d].dependencies, bdeps);

        // deps
        ctx.addLine("if (NOT CPPAN_LOCAL_BUILD)");
        ctx.increaseIndent();
        if (cc->pkg.empty())
        for (auto &dp : bdeps)
        {
            auto &p = dp.second;
            if (!p.flags[pfExecutable])
                ctx.addLine("get_configuration(config)");
            else
                ctx.addLine("get_configuration_exe(config)");
            ctx.addLine("set(current_dir " + normalize_path(p.getDirObj()) + ")");
            ctx.addLine("set(build_dir ${current_dir}/build/${config})");
            ctx.addLine("add_custom_command(TARGET " + cppan_dummy_target + " PRE_BUILD");
            ctx.increaseIndent();
            ctx.addLine("COMMAND ${CMAKE_COMMAND}");
            ctx.increaseIndent();
            ctx.addLine("-DTARGET_FILE=$<TARGET_FILE:" + p.target_name + ">");
            ctx.addLine("-DCONFIG=$<CONFIG>");
            ctx.addLine("-DBUILD_DIR=${build_dir}");
            ctx.addLine("-P " + normalize_path(p.getDirObj()) + "/" + non_local_build_file);
            ctx.decreaseIndent();
            ctx.decreaseIndent();
            ctx.addLine(")");
            ctx.addLine();
        }

        // post (copy)
        // no copy for non local builds
        if (cc->internal_options.current_package.empty())
        {
            ctx.addLine("if (CPPAN_BUILD_SHARED_LIBS)");
            ctx.increaseIndent();
            ctx.addLine("set(output_dir ${CMAKE_RUNTIME_OUTPUT_DIRECTORY})");
            ctx.addLine("if (MSVC OR XCODE)");
            ctx.addLine("    set(output_dir ${output_dir}/$<CONFIG>)");
            ctx.addLine("endif()");
            ctx.addLine("if (CPPAN_BUILD_OUTPUT_DIR)");
            ctx.addLine("    set(output_dir ${CPPAN_BUILD_OUTPUT_DIR})");
            ctx.addLine("endif()");
            ctx.addLine();

            for (auto &dp : bdeps)
            {
                ctx.addLine("add_custom_command(TARGET " + cppan_dummy_target + " POST_BUILD");
                ctx.increaseIndent();
                ctx.addLine("COMMAND ${CMAKE_COMMAND} -E copy_if_different");
                ctx.increaseIndent();
                ctx.addLine("$<TARGET_FILE:" + d.target_name + "> ${output_dir}/$<TARGET_FILE_NAME:" + d.target_name + ">");
                ctx.decreaseIndent();
                ctx.decreaseIndent();
                ctx.addLine(")");
                ctx.addLine();
            }

            ctx.decreaseIndent();
            ctx.addLine("endif()");
            ctx.addLine();
        }

        ctx.decreaseIndent();
        ctx.addLine("endif()");
        ctx.addLine();
    }

    ctx.addLine(config_delimeter);
    ctx.addLine();

    access_table->write_if_older(fn, ctx.getText());
}
