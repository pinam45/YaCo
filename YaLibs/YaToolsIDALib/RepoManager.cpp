//  Copyright (C) 2017 The YaCo Authors
//
//  This program is free software: you can redistribute it and/or modify
//  it under the terms of the GNU General Public License as published by
//  the Free Software Foundation, either version 3 of the License, or
//  (at your option) any later version.
//
//  This program is distributed in the hope that it will be useful,
//  but WITHOUT ANY WARRANTY; without even the implied warranty of
//  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
//  GNU General Public License for more details.
//
//  You should have received a copy of the GNU General Public License
//  along with this program.  If not, see <http://www.gnu.org/licenses/>.

#include "RepoManager.hpp"

#include "../YaGitLib/YaGitLib.hpp"
#include "Ida.h"
#include "Logger.h"
#include "Yatools.h"

#include <memory>
#include <sstream>
#include <ctime>
#include <regex>

#ifdef _MSC_VER
#   include <filesystem>
#else
#   include <experimental/filesystem>
#endif

namespace fs = std::experimental::filesystem;

#ifdef __EA64__
#define EA_PREFIX   "ll"
#define EA_SIZE     "16"
#else
#define EA_PREFIX   ""
#define EA_SIZE     "8"
#endif
#define EA_FMT      "%0" EA_SIZE EA_PREFIX "X"

#define LOG(LEVEL, FMT, ...) CONCAT(YALOG_, LEVEL)("repo_manager", (FMT "\n"), ## __VA_ARGS__)

#define GITREPO_TRY(call, msg) \
try { \
    call; \
} \
catch (const std::runtime_error& error) \
{ \
    warning(msg "\n\n%s", error.what()); \
}

static constexpr size_t truncate_commit_message_length = 4000;

static bool remove_substring(std::string& str, const std::string& substr)
{
    if (substr.empty())
        return false;

    const unsigned int pos = str.rfind(substr);
    if (pos != std::string::npos)
    {
        str.erase(pos, substr.size());
        return true;
    }

    return false;
}

namespace
{
    struct RepoManager
        : public IRepoManager
    {
        RepoManager(bool ida_is_interactive);

        bool ask_to_checkout_modified_files(bool repo_auto_sync) override;

        void ensure_git_globals() override;

        void add_auto_comment(ea_t ea, const std::string& text) override;

        bool repo_exists() override;

        void repo_init(const std::string& idb_filename, bool ask_for_remote = true) override;

        void repo_open(const std::string path) override;

        std::tuple<std::set<std::string>, std::set<std::string>, std::set<std::string>> repo_get_cache_files_status() override;

        std::string get_master_commit() override;
        std::string get_origin_master_commit() override;

        void fetch_origin() override;
        void fetch(const std::string& origin) override;

        void push_origin_master() override;

        void checkout_master() override;

        void check_valid_cache_startup() override;

        bool repo_commit(std::string commit_msg = "") override;

        //tmp
        GitRepo& get_repo() override;
        void new_repo(const std::string& path) override;

    private:
        bool ida_is_interactive_;

        GitRepo repo_;

        std::vector<std::tuple<std::string, std::string>> auto_comments_;
    };
}

RepoManager::RepoManager(bool ida_is_interactive)
    : ida_is_interactive_{ ida_is_interactive },
    repo_{ "." }
{

}

// TODO: move repo_auto_sync from python to RepoManager
bool RepoManager::ask_to_checkout_modified_files(bool repo_auto_sync)
{
    std::string modified_objects;
    bool checkout_head{ false };

    std::string original_idb = get_original_idb_name(database_idb);
    for (std::string modified_object : repo_.get_modified_objects())
    {
        if (modified_object == original_idb)
        {
            std::time_t now{ std::time(nullptr) };
            std::string date{ std::ctime(&now) };
            std::replace(date.begin(), date.end(), ' ', '_');
            std::replace(date.begin(), date.end(), ':', '_');
            std::string suffix = "_bkp_" + date;
            std::string new_idb = get_local_idb_name(original_idb, suffix);
            try
            {
                fs::copy_file(original_idb, new_idb);
            }
            catch (fs::filesystem_error error)
            {
                warning("Couldn't create backup idb file.\n\n%s", error.what());
                throw std::runtime_error(error);
            }
            checkout_head = true;
        }
        else
        {
            modified_objects += modified_object;
            modified_objects += '\n';
        }
    }

    if (!modified_objects.empty())
    {
        // modified_objects is now the message
        modified_objects += "\nhas been modified, this is not normal, do you want to checkout these files ? (Rebasing will be disabled if you answer no)";
        if (askyn_c(true, modified_objects.c_str()) != ASKBTN_NO)
        {
            repo_.checkout_head();
        }
        else
        {
            //repo_auto_sync = false;
            return false;
        }
    }

    if (checkout_head)
    {
        // checkout silently
        repo_.checkout_head();
    }

    return repo_auto_sync;
}

void RepoManager::ensure_git_globals()
{
    std::string userName{ repo_.config_get_string("user.name") };
    if (userName.empty())
    {
        do
        {
            const char* tmp = askstr(0, "username", "Entrer git user.name");
            userName = tmp != nullptr ? tmp : "";
        }
        while (userName.empty());
        GITREPO_TRY(repo_.config_set_string("user.name", userName), "Couldn't set git user name.");
    }

    std::string userEmail{ repo_.config_get_string("user.email") };
    if (userEmail.empty())
    {
        do
        {
            const char* tmp = askstr(0, "username@localdomain", "Entrer git user.email");
            userEmail = tmp != nullptr ? tmp : "";
        }
        while (userEmail.empty());
        GITREPO_TRY(repo_.config_set_string("user.email", userEmail), "Couldn't set git user email.");
    }
}

void RepoManager::add_auto_comment(ea_t ea, const std::string & text)
{
    std::string prefix;
    if (get_struc(ea))
    {
        if (get_struc_idx(ea) == BADADDR)
        {
            prefix += "stackframe '";
            qstring func_name;
            get_func_name2(&func_name, get_func_by_frame(ea));
            prefix += func_name.c_str();
            prefix += "'";
        }
        else
        {
            prefix += "structure '";
            prefix += get_struc_name(ea).c_str();
            prefix += "'";
        }
    }
    else if (get_enum_idx(ea) != BADADDR)
    {
        prefix += "enum '";
        prefix += get_enum_name(ea).c_str();
        prefix += "'";
    }
    else
    {
        prefix += ea_to_hex(ea);
        char foffset[100];
        if (a2funcoff(ea, foffset, sizeof(foffset)))
        {
            prefix += ',';
            prefix += foffset;
        }
    }
    auto_comments_.emplace_back(std::move(prefix), text);
}

bool RepoManager::repo_exists()
{
    bool is_directory = false;
    try
    {
        is_directory = fs::is_directory(fs::path{ ".git" });
    }
    catch (fs::filesystem_error) {}
    return is_directory;
}

void RepoManager::repo_init(const std::string& idb_filename, bool ask_for_remote)
{
    try
    {
        repo_ = GitRepo{ "." };
        repo_.init();
        ensure_git_globals();

        //add current IDB to repo
        repo_.add_file(idb_filename);

        //create an initial commit with IDB
        repo_.commit("Initial commit");
    }
    catch (std::runtime_error _error)
    {
        LOG(ERROR, "An error occured during repo init, error: %s", _error.what());
        error("An error occured during repo init, error: %s", _error.what());
        return;
    }

    if (ida_is_interactive_)
    {
        if (ask_for_remote)
        {
            const char* tmp = askstr(0, "ssh://gitolite@repo/", "Specify a remote origin :");
            std::string url = tmp != nullptr ? tmp : "";
            if (!url.empty())
            {
                try
                {
                    repo_.create_remote("origin", url);
                }
                catch (std::runtime_error _error)
                {
                    LOG(ERROR, "An error occured during remote creation, error: %s", _error.what());
                    error("An error occured during remote creation, error: %s", _error.what());
                    return;
                }

                if (!std::regex_match(url, std::regex("^ssh://.*"))) // add http/https to regex ? ("^((ssh)|(https?))://.*")
                {
                    fs::path path{ url };
                    if (!fs::exists(path))
                    {
                        if (askyn_c(true, "The target directory doesn't exist, do you want to create it ?") == ASKBTN_YES)
                        {
                            if (fs::create_directories(path))
                            {
                                GitRepo tmp_repo{ url };
                                tmp_repo.init_bare();//TODO
                            }
                            else
                            {
                                warning("Directory %s creation failed.", url.c_str());
                            }
                        }
                    }
                }
            }
        }
        copy_idb_to_local_file();
    }

    push_origin_master();
}

void RepoManager::repo_open(const std::string path)
{
    repo_ = GitRepo(path);
    GITREPO_TRY(repo_.init(), "Couldn't init repository.");
    ensure_git_globals();
}

std::tuple<std::set<std::string>, std::set<std::string>, std::set<std::string>> RepoManager::repo_get_cache_files_status()
{
    return std::make_tuple(
        repo_.get_untracked_objects_in_path("cache/"),
        repo_.get_deleted_objects_in_path("cache/"),
        repo_.get_modified_objects_in_path("cache/")
    );
}

std::string RepoManager::get_master_commit()
{
    std::string result;
    GITREPO_TRY(result = repo_.get_commit("master"), "Couldn't get commit from master.");
    return result;
}

std::string RepoManager::get_origin_master_commit()
{
    std::string result;
    GITREPO_TRY(result = repo_.get_commit("origin/master"), "Couldn't get commit from origin/master.");
    return result;
}

void RepoManager::fetch_origin()
{
    GITREPO_TRY(repo_.fetch(), "Couldn't fetch remote origin.");
}

void RepoManager::fetch(const std::string& origin)
{
    GITREPO_TRY(repo_.fetch(origin), "Couldn't fetch remote.");
}

void RepoManager::push_origin_master()
{
    const std::map<std::string, std::string> remotes{ repo_.get_remotes() };
    if (remotes.find(std::string("origin")) != remotes.end())
    {
        GITREPO_TRY(repo_.push("master", "master"), "Couldn't push to remote origin.");
    }
}

void RepoManager::checkout_master()
{
    GITREPO_TRY(repo_.checkout("master"), "Couldn't checkout master.");
}

void RepoManager::check_valid_cache_startup()
{
    LOG(INFO, "check valid cache startup");

    std::map<std::string, std::string> remotes;
    try
    {
        remotes = repo_.get_remotes();
    }
    catch (std::runtime_error error)
    {
        LOG(WARNING, "Couldn't get repo remotes, error: %s", error.what());
    }

    if (remotes.find("origin") == remotes.end())
    {
        LOG(WARNING, "origin not defined: ignoring origin and master sync check!");
    }
    else
    {
        if (repo_.get_commit("origin/master") != repo_.get_commit("master"))
        {
            LOG(WARNING, "Master and origin/master doesn't point to the same commit, please update your master.");
        }
    }

    try
    {
        fs::create_directory("cache");
    }
    catch (fs::filesystem_error){}

    fs::path idb_path{ database_idb };
    std::string idb_prefix{ idb_path.filename().string() };
    std::string idb_extension{ idb_path.extension().string() };
    remove_substring(idb_prefix, idb_extension);

    if (!std::regex_match(idb_prefix, std::regex{ ".*_local$" }))
    {
        std::string local_idb_name = idb_prefix + "_local" + idb_extension;
        bool local_idb_exist = false;
        try
        {
            local_idb_exist = fs::exists(local_idb_name);
        }
        catch (fs::filesystem_error) {}
        if (!local_idb_exist)
            copy_idb_to_local_file();

        if (ida_is_interactive_)
        {
            std::string msg = "To use YaCo you must name your IDB with _local suffix. YaCo will create one for you.\nRestart IDA and open ";
            msg += local_idb_name;
            msg += '.';
            database_flags |= DBFL_KILL;
            warning(msg.c_str());
            qexit(0);
        }
    }
}

bool RepoManager::repo_commit(std::string commit_msg)
{
    LOG(INFO, "committing changes");

    std::set<std::string> untracked_files{ repo_.get_untracked_objects_in_path("cache/") };
    std::set<std::string> modified_files{ repo_.get_modified_objects_in_path("cache/") };
    std::set<std::string> deleted_files{ repo_.get_deleted_objects_in_path("cache/") };

    if (untracked_files.empty() && modified_files.empty() && deleted_files.empty())
        return false;

    for (std::string f : untracked_files)
    {
        repo_.add_file(f);
        LOG(INFO, "added    %s", f.c_str());
    }
    for (std::string f : modified_files)
    {
        repo_.add_file(f);
        LOG(INFO, "modified %s", f.c_str());
    }
    for (std::string f : deleted_files)
    {
        repo_.remove_file(f);
        LOG(INFO, "deleted  %s", f.c_str());
    }

    size_t max_prefix_len = 0;
    size_t max_txt_len = 0;
    for (const std::tuple<std::string, std::string>& comment : auto_comments_)
    {
        max_prefix_len = std::max(std::get<0>(comment).size(), max_prefix_len);
        max_txt_len = std::max(std::get<1>(comment).size(), max_txt_len);
    }

    std::sort(auto_comments_.begin(), auto_comments_.end(), 
        [](const std::tuple<std::string, std::string>& a, const std::tuple<std::string, std::string>& b) {
        int cmp = std::get<0>(a).compare(std::get<0>(b));
        if (cmp == 0)
        {
            cmp = std::get<1>(a).compare(std::get<1>(b));
        }
        return cmp < 0;
    });

    size_t max_total_len = max_prefix_len + max_txt_len + 4; // for '[', ']', ' ', '\0'
    std::unique_ptr<char[]> buffer = std::make_unique<char[]>(max_total_len);
    if (commit_msg.empty())
    {
        bool need_trucate = false;
        for (const std::tuple<std::string, std::string>& comment : auto_comments_)
        {
            snprintf(buffer.get(), max_total_len, "[%-*s] %s", max_prefix_len, std::get<0>(comment).c_str(), std::get<1>(comment).c_str());
            commit_msg += buffer.get();
            commit_msg += '\n';
            need_trucate = commit_msg.size() > truncate_commit_message_length;
            if (need_trucate)
                break;
        }
        if (need_trucate)
        {
            commit_msg.erase(truncate_commit_message_length);
            commit_msg += "\n...truncated";
        }
    }

    if (commit_msg.empty())
        return false;

    try
    {
        repo_.commit(commit_msg);
    }
    catch (std::runtime_error _error)
    {
        LOG(ERROR, "An error occured during commit, error: %s", _error.what());
        error("An error occured during commit, error: %s", _error.what());
        return false;
    }
    auto_comments_.clear();

    return true;
}

GitRepo& RepoManager::get_repo()
{
    return repo_;
}

void RepoManager::new_repo(const std::string& path)
{
    repo_ = GitRepo{ path };
}

std::shared_ptr<IRepoManager> MakeRepoManager(bool ida_is_interactive)
{
    return std::make_shared<RepoManager>(ida_is_interactive);
}

std::string ea_to_hex(ea_t ea)
{
    char buffer[19]; // size for 0x%016X + \0
    std::snprintf(buffer, COUNT_OF(buffer), "0x" EA_FMT, ea);
    return std::string{ buffer };
}

std::string get_original_idb_name(const std::string& local_idb_name, const std::string& suffix)
{
    std::string orig_file_name{ fs::path{ local_idb_name }.filename().string() };

    if (suffix.empty())
        remove_substring(orig_file_name, "_local");
    else
        remove_substring(orig_file_name, suffix);

    return orig_file_name;
}

std::string get_local_idb_name(const std::string& original_idb_name, const std::string& suffix)
{
    fs::path idb_path{ original_idb_name };
    std::string idb_name{ idb_path.filename().string() };
    std::string idb_extension{ idb_path.extension().string() };
    remove_substring(idb_name, idb_extension);

    std::string local_idb_name{ idb_name };
    if (suffix.empty())
        local_idb_name += "_local";
    else
        local_idb_name += suffix;
    local_idb_name += idb_extension;

    return local_idb_name;
}

void remove_ida_temporary_files(const std::string& idb_path)
{
    std::string idb_no_ext{ idb_path };
    std::string idb_extension{ fs::path{ idb_path }.extension().string() };
    remove_substring(idb_no_ext, idb_extension);

    const char* extentions_to_delete[] = { ".id0", ".id1", ".id2", ".nam", ".til" };
    for (const char* ext : extentions_to_delete)
    {
        try
        {
            fs::remove(fs::path{ idb_no_ext + ext });
        }
        catch (fs::filesystem_error){}
    }
}

std::string copy_idb_to_local_file(const std::string& suffix)
{
    std::string local_file_name{ get_local_idb_name(database_idb, suffix) };
    save_database_ex(local_file_name.c_str(), 0);
    remove_ida_temporary_files(local_file_name);
    return local_file_name;
}

std::string copy_idb_to_original_file(const std::string& suffix)
{
    std::string orig_file_name{ get_original_idb_name(database_idb, suffix) };
    save_database_ex(orig_file_name.c_str(), 0);
    remove_ida_temporary_files(orig_file_name);
    return orig_file_name;
}