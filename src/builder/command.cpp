// Copyright (C) 2017-2018 Egor Pugin <egor.pugin@gmail.com>
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#define BOOST_THREAD_PROVIDES_EXECUTORS
#define BOOST_THREAD_PROVIDES_VARIADIC_THREAD
#define BOOST_THREAD_VERSION 5
#include <sw/builder/command.h>

#include "command_storage.h"
#include "db.h"
#include "program.h"
//#include "target.h"

#include <hash.h>
#include <filesystem.h>
#include <primitives/executor.h>
#include <primitives/templates.h>
#include <boost/algorithm/string.hpp>
#include <boost/thread/thread_pool.hpp>

#include <iostream>

#include <primitives/log.h>
DECLARE_STATIC_LOGGER(logger, "command");

namespace sw
{

CommandStorage &getCommandStorage()
{
    static CommandStorage fs;
    return fs;
}

CommandStorage::CommandStorage()
{
    load();
}

CommandStorage::~CommandStorage()
{
    try
    {
        save();
    }
    catch (...)
    {
        LOG_ERROR(logger, "Error during file db save");
    }
}

void CommandStorage::load()
{
    getDb().load(commands);
}

void CommandStorage::save()
{
    getDb().save(commands);
}

bool CommandStorage::isOutdated(const sw::builder::Command &c)
{
    // TODO: rewrite explain if needed

    bool changed = false;

    // always check program and all deps are known
    changed = File(c.program).isChanged();
    for (auto &i : c.inputs)
        changed = File(i).isChanged();
    for (auto &i : c.outputs)
        changed = File(i).isChanged();

    auto k = std::hash<sw::builder::Command>()(c);
    auto r = commands.insert_ptr(k, 0);
    if (r.second)
    {
        // we have insertion, no previous value available
        // so outdated
        EXPLAIN_OUTDATED("command", true, "new command: " + c.print(), c.getName());
        changed = true;
    }

    if (c.always)
    {
        //EXPLAIN_OUTDATED("command", true, "always build", c.getName());
        changed = true;
    }

    //auto h = c.calculateFilesHash();
    //return *r.first != h;

    // we have this command, now check if it outdated
    /*if (changed)
    {
        EXPLAIN_OUTDATED("command", true, "program changed", c.getName());
    }*/

    /*if (std::any_of(c.inputs.begin(), c.inputs.end(),
                    [](auto &d) { return File(d).isChanged(); }) ||
        std::any_of(c.outputs.begin(), c.outputs.end(),
                    [](auto &d) { return File(d).isChanged(); }))
    {
        EXPLAIN_OUTDATED("command", true, "i/o file is changed", c.getName());
        changed = true;
    }*/

    //EXPLAIN_OUTDATED("command", false, "ok", c.getName());

    // comment to turn on command hashes feature
    return changed;

    // we don't see changes, now check command hash
    if (!r.second)
        return *r.first != c.calculateFilesHash();

    return false;
}

namespace builder
{

bool Command::isOutdated() const
{
    return getCommandStorage().isOutdated(*this);
}

size_t Command::getHash() const
{
    if (hash != 0)
        return hash;

    auto h = std::hash<path>()(program);
    for (auto &a : args)
        h ^= std::hash<String>()(a);
    //hash_combine(h, std::hash<String>()(a));
    return hash = h;
}

size_t Command::calculateFilesHash() const
{
    auto h = getHash();
    hash_combine(h, File(program).getFileRecord().getHash());
    for (auto &i : inputs)
        hash_combine(h, File(i).getFileRecord().getHash());
    for (auto &i : outputs)
        hash_combine(h, File(i).getFileRecord().getHash());
    return h;
}

void Command::updateFilesHash() const
{
    auto h = calculateFilesHash();
    auto k = std::hash<Command>()(*this);
    auto r = getCommandStorage().commands.insert_ptr(k, h);
    if (!r.second)
        *r.first = h;
}

void Command::clean() const
{
    error_code ec;
    for (auto &o : outputs)
        fs::remove(o, ec);
    for (auto &o : intermediate)
        fs::remove(o, ec);
}

path Command::getProgram() const
{
    path p;
    if (base)
    {
        p = base->file;
        if (p.empty())
            throw std::runtime_error("Empty program from base program");
    }
    /*else if (dependency)
    {
        if (!dependency->target)
            throw std::runtime_error("Command dependency target was not resolved");
        p = dependency->target->getOutputFile();
        if (p.empty())
            throw std::runtime_error("Empty program from package: " + dependency->target->getPackage().target_name);
    }*/
    else
    {
        p = program;
        if (p.empty())
            throw std::runtime_error("Empty program: was not set");
    }
    return p;
}

void Command::addInput(const path &p)
{
    inputs.insert(p);
}

void Command::addIntermediate(const path &p)
{
    intermediate.insert(p);
    auto &r = File(p).getFileRecord();
    if (!r.generator || r.generator->executed) // || hash = hash new
        r.generator = shared_from_this();
}

void Command::addOutput(const path &p)
{
    outputs.insert(p);
    auto &r = File(p).getFileRecord();
    if (!r.generator || r.generator->executed) // || hash = hash new
        r.generator = shared_from_this();
}

void Command::addInput(const Files &files)
{
    for (auto &f : files)
        addInput(f);
}

void Command::addIntermediate(const Files &files)
{
    for (auto &f : files)
        addIntermediate(f);
}

void Command::addOutput(const Files &files)
{
    for (auto &f : files)
        addOutput(f);
}

path Command::redirectStdout(const path &p)
{
    out.file = p;
    addOutput(p);
    return p;
}

path Command::redirectStderr(const path &p)
{
    err.file = p;
    addOutput(p);
    return p;
}

void Command::addInputOutputDeps()
{
    for (auto &p : inputs)
    {
        File f(p);
        if (f.isGenerated())
            dependencies.insert(f.getFileRecord().generator);
        else
        {
            // do we really need this?
            //auto deps = f.gatherDependentGenerators();
            //dependencies.insert(deps.begin(), deps.end());
        }
    }
    // do we really need this?
    for (auto &p : outputs)
    {
        File f(p);
        f.addExplicitDependency(inputs);
    }
}

void Command::prepare()
{
    if (prepared)
        return;

    program = getProgram();
    getHash();

    // add redirected generated files
    if (!out.file.empty())
        addOutput(out.file);
    if (!err.file.empty())
        addOutput(err.file);

    // add more deps
    if (File(program).isGenerated())
        dependencies.insert(File(program).getFileRecord().generator);
    addInputOutputDeps();

    prepared = true;
}

void Command::execute1(std::error_code *ec)
{
    prepare();

    if (getName().find("warnless.c") != -1)
    {
        int a = 5;
        a++;
    }

    if (!isOutdated())
        return;

    if (executed)
        throw std::logic_error("Trying to execute command twice: " + getName());

    printLog();

    if (remove_outputs_before_execution)
    {
        // Some programs won't update their binaries even in case of updated sources/deps.
        // E.g., msvc bug: https://developercommunity.visualstudio.com/content/problem/97608/libexe-does-not-update-import-library.html
        error_code ec;
        for (auto &o : outputs)
            fs::remove(o, ec);
    }

    //static std::atomic_int n = 0;
    //LOG_INFO(logger, "command #" << ++n << " is outdated: " + getName());

    // check our resources
    auto rp = getResourcePool();
    if (rp)
        rp->lock();
    SCOPE_EXIT
    {
        if (rp)
            rp->unlock();
    };

    // Try to construct command line first.
    // Some systems have limitation on its length.

    auto escape_cmd_arg = [](auto &a)
    {
        boost::replace_all(a, "\\", "\\\\");
        boost::replace_all(a, "\"", "\\\"");
        return a;
    };

    path rsp_file;
    auto args_saved = args;
    if (use_response_files && needsResponseFile())
    {
        rsp_file = get_temp_filename() / "rsp";
        rsp_file += ".rsp";
        String rsp;
        for (auto &a : args)
#ifdef _WIN32
            rsp += "\"" + escape_cmd_arg(a) + "\"\n";
#else
            rsp += "\"" + escape_cmd_arg(a) + "\"\n";
#endif
        args.clear();
        args.push_back("@" + rsp_file.string());
        write_file(rsp_file, rsp);
    }

    SCOPE_EXIT
    {
        if (rsp_file.empty())
            return;
        error_code ec;
        fs::remove(rsp_file, ec);
    };

    auto make_error_string = [this, &rsp_file, &args_saved, &escape_cmd_arg](const String &e)
    {
        executed = true;
        postProcess(false);

        String s = "When building: " + getName();
        if (!out.text.empty())
            s += "\n" + boost::trim_copy(out.text);
        if (!err.text.empty())
            s += "\n" + boost::trim_copy(err.text);
        s += "\n";
        s += e;
        boost::trim(s);
        if (!rsp_file.empty())
        {
            s += "\n";
            s += "full command:\n";
            s += "\"" + program.u8string() + "\" ";
            for (auto &a : args_saved)
                s += "\"" + escape_cmd_arg(a) + "\" ";
            s.resize(s.size() - 1);
            //s += "working directory:\n";
            //s += "\"" + working_directory.u8string() + "\"";
        }
        return s;
    };

    //LOG_INFO(logger, print());
    LOG_TRACE(logger, print());

    try
    {
        if (ec)
        {
            Base::execute(*ec);
            if (ec)
            {
                // TODO: save error string
                make_error_string("FIXME");
                return;
            }
        }
        else
            Base::execute();

        executed = true;
        postProcess(); // process deps

        // force outputs update
        /*for (auto &i : inputs)
        {
            auto &fr = f.getFileRecord();
            fr.refreshed = false;
            fr.isChanged();
        }*/
        for (auto &i : intermediate)
        {
            File f(i);
            /*if (!fs::exists(i))
                f.getFileRecord().flags.set(ffNotExists);
            else*/
            //f.getFileRecord().load();
            auto &fr = f.getFileRecord();
            fr.refreshed = false;
            fr.isChanged();
        }
        for (auto &i : outputs)
        {
            File f(i);
            /*if (!fs::exists(i))
                f.getFileRecord().flags.set(ffNotExists);
            else*/
            //f.getFileRecord().load();
            auto &fr = f.getFileRecord();
            fr.refreshed = false;
            fr.isChanged();
        }

        updateFilesHash();
    }
    catch (std::exception &e)
    {
        throw std::runtime_error(make_error_string(e.what()));
    }
}

bool Command::needsResponseFile() const
{
    // 3 = 1 + 2 = space + quotes
    size_t sz = program.u8string().size() + 3;
    for (auto &a : args)
        sz += a.size() + 3;
    return sz >
#ifdef _WIN32
        8100 // win have 8192 limit, we take a bit fewer symbols
#else
        8100
#endif
        ;
}

String Command::getName(bool short_name) const
{
    if (name.empty())
    {
        if (!outputs.empty())
        {
            String s = "Generating: ";
            for (auto &o : outputs)
                s += "\"" + normalize_path(o) + "\", ";
            s.resize(s.size() - 2);
            return s;
        }
        return std::to_string((uint64_t)this);
    }
    //return "Building: \"" + (short_name ? name_short : name) + "\"";
    return "\"" + (short_name ? name_short : name) + "\"";
}

void Command::printLog() const
{
    if (silent)
        return;
    static Executor eprinter(1);
    if (current_command)
    {
        std::string msg = "[" + std::to_string((*current_command)++) + "/" + std::to_string(total_commands) + "] " + getName();
        eprinter.push([msg]
        {
            LOG_INFO(logger, msg);
        });
    }
}

void Command::setProgram(const path &p)
{
    program = p;
}

void Command::setProgram(const Program &p)
{
    base = &p;
}

}

void ExecuteCommand::prepare()
{
    if (prepared)
        return;
    addInputOutputDeps();
    prepared = true;
}

bool ExecuteCommand::isOutdated() const
{
    if (std::none_of(inputs.begin(), inputs.end(),
        [](auto &d) { return File(d).isChanged(); }) &&
        std::none_of(outputs.begin(), outputs.end(),
            [](auto &d) { return File(d).isChanged(); }))
        return false;
    return true;
}

void ExecuteCommand::execute()
{
    if (always)
    {
        f();
        return;
    }

    if (!isOutdated())
        return;

    printLog();

    f();

    // force outputs update
    for (auto &o : outputs)
        File(o).getFileRecord().load();
}

}