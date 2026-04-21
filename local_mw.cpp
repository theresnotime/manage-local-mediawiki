#include <array>
#include <chrono>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <future>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#ifndef _WIN32
#include <csignal>
#include <sys/wait.h>
#endif

namespace fs = std::filesystem;

// Version is injected at compile time via -DAPP_VERSION
#ifndef APP_VERSION
#define APP_VERSION "dev-unknown"
#endif
const std::string VERSION = APP_VERSION;
const std::string SOURCE_URL =
    "https://github.com/theresnotime/manage-local-mediawiki";

bool g_verbose = false;
bool g_reportOnly = false;
bool g_autoYes = false;
bool g_updateMode = false;
std::string g_updateType;
std::string g_updateName;
std::string g_reportFile;
int g_commandTimeoutSeconds = 30;
int g_pullTimeoutSeconds = 120;
int g_gerritDelaySeconds = 1;
std::mutex g_coutMutex;
std::mutex g_gerritRateLimitMutex;
std::mutex g_gerritCacheMutex;
std::chrono::steady_clock::time_point g_lastGerritInteraction =
    std::chrono::steady_clock::time_point::min();
std::unordered_map<std::string, bool> g_gerritRepoCache;

/**
 * Log a verbose message (thread-safe)
 *
 * @param message The message to log
 */
void logVerbose(const std::string &message) {
  if (!g_verbose)
    return;
  std::lock_guard<std::mutex> lock(g_coutMutex);
  std::cout << message;
  if (!message.empty() && message.back() != '\n') {
    std::cout << '\n';
  }
  std::cout.flush();
}

/**
 * Write output to console and optionally to a report file.
 *
 * @param message The message to write.
 * @param reportStream Optional output file stream for the report.
 */
void writeOutput(const std::string &message,
                 std::ofstream *reportStream = nullptr) {
  std::cout << message;
  if (reportStream && reportStream->is_open()) {
    *reportStream << message;
  }
}

/**
 * Prompt user for yes/no confirmation (thread-safe)
 *
 * @param message The prompt message
 */
bool promptForConfirmation(const std::string &message) {
  std::lock_guard<std::mutex> lock(g_coutMutex);
  std::cout << message << " [y/N]: ";
  std::cout.flush();
  std::string response;
  std::getline(std::cin, response);
  return !response.empty() && (response[0] == 'y' || response[0] == 'Y');
}

struct RepoStatus {
  std::string name;
  std::string type;
  bool isRepo;
  bool hasUpdates;
  std::string currentBranch;
  int behindBy;
  std::string error;
  bool pulled;
  bool hadUncommittedChanges;
  std::string pullError;
};

// Forward declarations for timeout-aware command and git helpers.
std::string execCommand(const std::string &cmd, int timeoutSeconds,
                        bool *timedOut, int *exitCode);
std::string getCurrentBranch(const fs::path &repoPath, bool &timedOut);
bool fetchUpdates(const fs::path &repoPath, std::string &errorMsg,
                  bool &timedOut);
int checkBehindCommits(const fs::path &repoPath, const std::string &branch,
                       bool &timedOut);
bool hasUncommittedChanges(const fs::path &repoPath, bool &timedOut);
bool performGitPull(const fs::path &repoPath, std::string &errorMsg,
                    bool &timedOut);
bool isGerritRepo(const fs::path &repoPath);
void throttleGerritInteraction(const fs::path &repoPath,
                               const std::string &operation);

/**
 * Execute a command and capture its output
 *
 * @param cmd The command to execute
 * @return The command output as a string
 */
std::string execCommand(const std::string &cmd) {
  return execCommand(cmd, 0, nullptr, nullptr);
}

/**
 * Escape a string for safe use as a single shell argument.
 *
 * @param value Input value
 * @return Shell-escaped value wrapped in single quotes
 */
std::string shellEscape(const std::string &value) {
  std::string escaped;
  escaped.reserve(value.size() + 2);
  escaped.push_back('\'');
  for (char c : value) {
    if (c == '\'') {
      escaped += "'\"'\"'";
    } else {
      escaped.push_back(c);
    }
  }
  escaped.push_back('\'');
  return escaped;
}

/**
 * Build a command string that enforces a timeout using perl alarm.
 *
 * @param cmd The command to execute
 * @param timeoutSeconds Timeout in seconds; <= 0 means no timeout
 * @return Wrapped command string
 */
std::string buildTimedCommand(const std::string &cmd, int timeoutSeconds) {
  if (timeoutSeconds <= 0) {
    return cmd;
  }

#ifdef _WIN32
  return cmd;
#else
  std::ostringstream oss;
  oss << "perl -e 'alarm shift; exec @ARGV' " << timeoutSeconds
      << " /bin/sh -c " << shellEscape(cmd);
  return oss.str();
#endif
}

/**
 * Execute a command, optionally with timeout and status metadata.
 *
 * @param cmd The command to execute
 * @param timeoutSeconds Timeout in seconds; <= 0 means no timeout
 * @param timedOut Optional output flag set true when timeout occurs
 * @param exitCode Optional output command exit code
 * @return The command output as a string
 */
std::string execCommand(const std::string &cmd, int timeoutSeconds,
                        bool *timedOut, int *exitCode) {
  if (g_verbose) {
    std::ostringstream oss;
    oss << "  [CMD] " << cmd;
    if (timeoutSeconds > 0) {
      oss << " (timeout=" << timeoutSeconds << "s)";
    }
    logVerbose(oss.str());
  }

  if (timedOut) {
    *timedOut = false;
  }
  if (exitCode) {
    *exitCode = -1;
  }

  std::string finalCommand = buildTimedCommand(cmd, timeoutSeconds);
  std::array<char, 128> buffer;
  std::string result;
  FILE *pipe = popen(finalCommand.c_str(), "r");
  if (!pipe) {
    if (g_verbose) {
      logVerbose("  [ERROR] Failed to execute command");
    }
    return "";
  }
  while (fgets(buffer.data(), buffer.size(), pipe) != nullptr) {
    result += buffer.data();
  }
  int status = pclose(pipe);

  bool didTimeout = false;
  int code = -1;

#ifndef _WIN32
  if (status != -1) {
    if (WIFEXITED(status)) {
      code = WEXITSTATUS(status);
    } else if (WIFSIGNALED(status)) {
      int sig = WTERMSIG(status);
      code = 128 + sig;
      if (sig == SIGALRM) {
        didTimeout = true;
      }
    }
  }
#else
  code = status;
#endif

  // perl alarm timeout commonly propagates as exit code 142
  if (code == 142) {
    didTimeout = true;
  }

  if (timedOut) {
    *timedOut = didTimeout;
  }
  if (exitCode) {
    *exitCode = code;
  }

  if (didTimeout) {
    std::ostringstream timeoutMsg;
    timeoutMsg << "[TIMEOUT] Command exceeded " << timeoutSeconds << "s";
    if (!result.empty() && result.back() != '\n') {
      result.push_back('\n');
    }
    result += timeoutMsg.str() + "\n";
  }

  if (g_verbose && !result.empty()) {
    if (result.back() != '\n') {
      result.push_back('\n');
    }
    logVerbose("  [OUTPUT] " + result);
  }
  return result;
}

/**
 * Check if a directory is a MediaWiki installation
 *
 * @param path The directory path to check
 * @return true if it is a MediaWiki installation, false otherwise
 */
bool isMediaWikiDirectory(const fs::path &path) {
  // Check for key MediaWiki files and directories
  bool hasIndexPhp = fs::exists(path / "index.php");
  bool hasApiPhp = fs::exists(path / "api.php");
  bool hasIncludesDir =
      fs::exists(path / "includes") && fs::is_directory(path / "includes");
  bool hasExtensionsDir =
      fs::exists(path / "extensions") && fs::is_directory(path / "extensions");
  bool hasSkinsDir =
      fs::exists(path / "skins") && fs::is_directory(path / "skins");

  // A valid MediaWiki installation should have at least these core components
  return hasIndexPhp && hasApiPhp && hasIncludesDir && hasExtensionsDir &&
         hasSkinsDir;
}

/**
 * Check if a directory is a git repository
 *
 * @param path The directory path to check
 * @return true if it is a git repository, false otherwise
 */
bool isGitRepo(const fs::path &path) { return fs::exists(path / ".git"); }

/**
 * Check whether repository origin appears to be a Gerrit remote.
 *
 * @param repoPath The repository path
 * @return true if origin URL contains "gerrit", false otherwise
 */
bool isGerritRepo(const fs::path &repoPath) {
  const std::string key = repoPath.string();
  {
    std::lock_guard<std::mutex> lock(g_gerritCacheMutex);
    auto it = g_gerritRepoCache.find(key);
    if (it != g_gerritRepoCache.end()) {
      return it->second;
    }
  }

  std::string cmd = "cd \"" + repoPath.string() +
                    "\" && git remote get-url origin 2>/dev/null";
  std::string remoteUrl =
      execCommand(cmd, g_commandTimeoutSeconds, nullptr, nullptr);
  bool isGerrit = remoteUrl.find("gerrit") != std::string::npos;

  {
    std::lock_guard<std::mutex> lock(g_gerritCacheMutex);
    g_gerritRepoCache[key] = isGerrit;
  }

  return isGerrit;
}

/**
 * Throttle Gerrit interactions across all worker threads.
 *
 * @param repoPath The repository path
 * @param operation Operation name for verbose logs
 */
void throttleGerritInteraction(const fs::path &repoPath,
                               const std::string &operation) {
  if (g_gerritDelaySeconds <= 0 || !isGerritRepo(repoPath)) {
    return;
  }

  const auto delay = std::chrono::seconds(g_gerritDelaySeconds);
  std::unique_lock<std::mutex> lock(g_gerritRateLimitMutex);
  if (g_lastGerritInteraction != std::chrono::steady_clock::time_point::min()) {
    auto now = std::chrono::steady_clock::now();
    auto elapsed = now - g_lastGerritInteraction;
    if (elapsed < delay) {
      auto waitTime = std::chrono::duration_cast<std::chrono::milliseconds>(
          delay - elapsed);
      if (g_verbose) {
        logVerbose("  [RATE LIMIT] Waiting " +
                   std::to_string(waitTime.count()) + "ms before Gerrit " +
                   operation);
      }
      std::this_thread::sleep_for(delay - elapsed);
    }
  }
  g_lastGerritInteraction = std::chrono::steady_clock::now();
}

/**
 * Get the current branch name
 *
 * @param repoPath The repository path
 * @return The current branch name, or empty string on error
 */
std::string getCurrentBranch(const fs::path &repoPath) {
  bool timedOut = false;
  return getCurrentBranch(repoPath, timedOut);
}

/**
 * Get the current branch name
 *
 * @param repoPath The repository path
 * @param timedOut Output flag indicating timeout
 * @return The current branch name, or empty string on error
 */
std::string getCurrentBranch(const fs::path &repoPath, bool &timedOut) {
  std::string cmd = "cd \"" + repoPath.string() +
                    "\" && git rev-parse --abbrev-ref HEAD 2>/dev/null";
  int exitCode = 0;
  std::string branch =
      execCommand(cmd, g_commandTimeoutSeconds, &timedOut, &exitCode);

  if (timedOut || exitCode != 0) {
    return "";
  }

  // Remove trailing newline
  if (!branch.empty() && branch.back() == '\n') {
    branch.pop_back();
  }
  return branch;
}

/**
 * Fetch updates from remote
 *
 * @param repoPath The repository path
 * @return true if fetch succeeded, false if error or fatal messages were
 * detected
 */
bool fetchUpdates(const fs::path &repoPath) {
  std::string errorMsg;
  bool timedOut = false;
  return fetchUpdates(repoPath, errorMsg, timedOut);
}

/**
 * Fetch updates from remote
 *
 * @param repoPath The repository path
 * @param errorMsg Detailed error or timeout message
 * @param timedOut Output flag indicating timeout
 * @return true if fetch succeeded, false otherwise
 */
bool fetchUpdates(const fs::path &repoPath, std::string &errorMsg,
                  bool &timedOut) {
  throttleGerritInteraction(repoPath, "fetch");
  std::string cmd = "cd \"" + repoPath.string() + "\" && git fetch 2>&1";
  int exitCode = 0;
  std::string output =
      execCommand(cmd, g_commandTimeoutSeconds, &timedOut, &exitCode);

  if (timedOut) {
    errorMsg = "git fetch timed out after " +
               std::to_string(g_commandTimeoutSeconds) + "s";
    return false;
  }

  if (exitCode != 0 || output.find("error") != std::string::npos ||
      output.find("fatal") != std::string::npos) {
    errorMsg = output.empty() ? "git fetch failed" : output;
    return false;
  }

  return true;
}

/**
 * Check if local branch is behind remote
 *
 * @param repoPath The repository path
 * @param branch The branch name to check
 * @return Number of commits behind, or -1 on error
 */
int checkBehindCommits(const fs::path &repoPath, const std::string &branch) {
  bool timedOut = false;
  return checkBehindCommits(repoPath, branch, timedOut);
}

/**
 * Check if local branch is behind remote
 *
 * @param repoPath The repository path
 * @param branch The branch name to check
 * @param timedOut Output flag indicating timeout
 * @return Number of commits behind, or -1 on error
 */
int checkBehindCommits(const fs::path &repoPath, const std::string &branch,
                       bool &timedOut) {
  std::string cmd = "cd \"" + repoPath.string() +
                    "\" && git rev-list --count HEAD..origin/" + branch +
                    " 2>/dev/null";
  int exitCode = 0;
  std::string result =
      execCommand(cmd, g_commandTimeoutSeconds, &timedOut, &exitCode);

  if (timedOut || exitCode != 0 || result.empty()) {
    return -1; // Error or no tracking branch
  }

  try {
    return std::stoi(result);
  } catch (...) {
    return -1;
  }
}

/**
 * Check if repository has uncommitted changes
 *
 * @param repoPath The repository path
 * @return true if there are uncommitted changes, false otherwise
 */
bool hasUncommittedChanges(const fs::path &repoPath) {
  bool timedOut = false;
  return hasUncommittedChanges(repoPath, timedOut);
}

/**
 * Check if repository has uncommitted changes
 *
 * @param repoPath The repository path
 * @param timedOut Output flag indicating timeout
 * @return true if there are uncommitted changes, false otherwise
 */
bool hasUncommittedChanges(const fs::path &repoPath, bool &timedOut) {
  std::string cmd =
      "cd \"" + repoPath.string() + "\" && git status --porcelain 2>/dev/null";
  int exitCode = 0;
  std::string result =
      execCommand(cmd, g_commandTimeoutSeconds, &timedOut, &exitCode);
  if (timedOut || exitCode != 0) {
    return false;
  }
  return !result.empty();
}

/**
 * Performs a git pull operation on the specified repository.
 *
 * @param repoPath The filesystem path to the git repository.
 * @param errorMsg Reference to a string that will contain error output if the
 * operation fails.
 * @return true if git pull succeeded, false if error or fatal messages were
 * detected.
 */
bool performGitPull(const fs::path &repoPath, std::string &errorMsg) {
  bool timedOut = false;
  return performGitPull(repoPath, errorMsg, timedOut);
}

/**
 * Performs a git pull operation on the specified repository.
 *
 * @param repoPath The filesystem path to the git repository.
 * @param errorMsg Reference to a string that will contain error output if the
 * operation fails.
 * @param timedOut Output flag indicating timeout
 * @return true if git pull succeeded, false otherwise.
 */
bool performGitPull(const fs::path &repoPath, std::string &errorMsg,
                    bool &timedOut) {
  throttleGerritInteraction(repoPath, "pull");
  std::string cmd = "cd \"" + repoPath.string() + "\" && git pull 2>&1";
  int exitCode = 0;
  std::string output =
      execCommand(cmd, g_pullTimeoutSeconds, &timedOut, &exitCode);

  if (timedOut) {
    errorMsg = "git pull timed out after " +
               std::to_string(g_pullTimeoutSeconds) + "s";
    return false;
  }

  if (exitCode != 0 || output.find("error") != std::string::npos ||
      output.find("fatal") != std::string::npos) {
    errorMsg = output.empty() ? "git pull failed" : output;
    return false;
  }

  return true;
}

/**
 * Check a repository for updates and optionally pull
 *
 * @param repoPath The repository path
 * @param type The type of repository (core, extension, skin)
 * @return The repository status
 */
RepoStatus checkRepository(const fs::path &repoPath, const std::string &type) {
  if (g_verbose) {
    std::ostringstream oss;
    oss << "\n[CHECKING] " << repoPath.filename().string() << " (" << type
        << ")\n";
    oss << "  Path: " << repoPath.string() << "\n";
    logVerbose(oss.str());
  }

  RepoStatus status;
  status.name = repoPath.filename().string();
  status.type = type;
  status.isRepo = isGitRepo(repoPath);
  status.hasUpdates = false;
  status.behindBy = 0;
  status.pulled = false;
  status.hadUncommittedChanges = false;

  if (!status.isRepo) {
    status.error = "Not a git repository";
    if (g_verbose) {
      logVerbose("  [SKIP] Not a git repository");
    }
    return status;
  }

  // Get current branch
  if (g_verbose) {
    logVerbose("  [STEP] Getting current branch...");
  }
  bool branchTimedOut = false;
  status.currentBranch = getCurrentBranch(repoPath, branchTimedOut);
  if (status.currentBranch.empty()) {
    status.error = branchTimedOut ? "Timed out determining branch"
                                  : "Could not determine branch";
    if (g_verbose) {
      logVerbose("  [ERROR] " + status.error);
    }
    return status;
  }
  if (g_verbose) {
    logVerbose("  [INFO] Current branch: " + status.currentBranch);
  }

  // Fetch updates
  if (g_verbose) {
    logVerbose("  [STEP] Fetching updates from remote...");
  }
  bool fetchTimedOut = false;
  std::string fetchError;
  if (!fetchUpdates(repoPath, fetchError, fetchTimedOut)) {
    status.error = fetchTimedOut ? fetchError : "Failed to fetch updates";
    if (!fetchError.empty() && !fetchTimedOut) {
      status.error += ": " + fetchError;
    }
    if (g_verbose) {
      logVerbose("  [ERROR] " + status.error);
    }
    return status;
  }

  // Check if behind
  if (g_verbose) {
    logVerbose("  [STEP] Checking commits behind remote...");
  }
  bool behindTimedOut = false;
  status.behindBy =
      checkBehindCommits(repoPath, status.currentBranch, behindTimedOut);
  if (behindTimedOut) {
    status.error = "Timed out checking commits behind remote";
    if (g_verbose) {
      logVerbose("  [ERROR] " + status.error);
    }
    return status;
  }

  // Check for uncommitted changes on all repos
  if (g_verbose) {
    logVerbose("  [STEP] Checking for uncommitted changes...");
  }
  bool uncommittedTimedOut = false;
  status.hadUncommittedChanges =
      hasUncommittedChanges(repoPath, uncommittedTimedOut);
  if (uncommittedTimedOut) {
    status.error = "Timed out checking uncommitted changes";
    if (g_verbose) {
      logVerbose("  [ERROR] " + status.error);
    }
    return status;
  }
  if (status.hadUncommittedChanges && g_verbose) {
    logVerbose("  [WARNING] Repository has uncommitted changes!");
  }

  if (status.behindBy > 0) {
    status.hasUpdates = true;
    if (g_verbose) {
      logVerbose("  [RESULT] Behind by " + std::to_string(status.behindBy) +
                 " commit(s)");
    }

    // Perform git pull if conditions are met
    // Skip auto-pull in update mode (updateSingleRepo handles it)
    bool shouldPull =
        !g_reportOnly && !g_updateMode &&
        (status.currentBranch == "master" || status.currentBranch == "main");

    if (shouldPull) {
      // Prompt for confirmation unless --yes was passed
      bool userConfirmed = g_autoYes;
      if (!g_autoYes) {
        std::ostringstream prompt;
        prompt << "\nPull updates for '" << status.name << "' (" << status.type
               << ", " << status.behindBy << " commit"
               << (status.behindBy > 1 ? "s" : "") << " behind)";
        if (status.hadUncommittedChanges) {
          prompt << "\n  ⚠️  WARNING: Has uncommitted changes!";
        }
        prompt << "\n  ";
        userConfirmed = promptForConfirmation(prompt.str());
      }

      if (userConfirmed) {
        if (g_verbose) {
          logVerbose("  [STEP] Performing git pull...");
        }
        bool pullTimedOut = false;
        if (performGitPull(repoPath, status.pullError, pullTimedOut)) {
          status.pulled = true;
          if (g_verbose) {
            logVerbose("  [SUCCESS] Git pull completed");
          }
        } else {
          if (pullTimedOut) {
            status.pullError = "git pull timed out after " +
                               std::to_string(g_pullTimeoutSeconds) + "s";
          }
          if (g_verbose) {
            logVerbose("  [ERROR] Git pull failed: " + status.pullError);
          }
        }
      } else {
        if (g_verbose) {
          logVerbose("  [INFO] User declined pull");
        }
        // Keep hasUpdates true but don't pull
      }
    }
  } else if (status.behindBy < 0) {
    status.error = "No tracking branch or error checking";
    if (g_verbose) {
      logVerbose("  [WARNING] No tracking branch or error checking");
    }
  } else {
    if (g_verbose) {
      logVerbose("  [RESULT] Up to date");
    }
  }

  return status;
}

/**
 * Count the number of directories in a given path
 *
 * @param dirPath The directory path to scan
 * @return The count of directories
 */
int countDirectories(const fs::path &dirPath) {
  int count = 0;
  if (fs::exists(dirPath) && fs::is_directory(dirPath)) {
    for (const auto &entry : fs::directory_iterator(dirPath)) {
      if (entry.is_directory()) {
        count++;
      }
    }
  }
  return count;
}

struct Statistics {
  int upToDate = 0;
  int hasUpdates = 0;
  int errors = 0;
};

/**
 * Calculate statistics from repository statuses
 *
 * @param results The vector of repository statuses
 * @return The calculated statistics
 */
Statistics calculateStats(const std::vector<RepoStatus> &results) {
  Statistics stats;
  for (const auto &status : results) {
    if (!status.isRepo || !status.error.empty()) {
      stats.errors++;
    } else if (status.hasUpdates) {
      stats.hasUpdates++;
    } else {
      stats.upToDate++;
    }
  }
  return stats;
}

/**
 * Print verbose directory header
 *
 * @param dirType The type of directory (extensions, skins)
 * @param dirPath The directory path
 */
void printVerboseDirectoryHeader(const std::string &dirType,
                                 const fs::path &dirPath) {
  if (!g_verbose)
    return;
  std::cout << "\n" << std::string(80, '=') << "\n";
  std::cout << "Scanning " << dirType << " directory: " << dirPath.string()
            << "\n";
  std::cout << std::string(80, '=') << "\n";
}

/**
 * Scan a directory for repositories
 *
 * @param dirPath The directory path to scan
 * @param type The type of repositories (extension, skin)
 * @return A vector of repository statuses
 */
std::vector<RepoStatus> scanDirectory(const fs::path &dirPath,
                                      const std::string &type) {
  std::vector<RepoStatus> results;

  if (!fs::exists(dirPath) || !fs::is_directory(dirPath)) {
    return results;
  }

  const unsigned int maxThreads =
      std::max(1u, std::thread::hardware_concurrency());
  std::vector<std::future<RepoStatus>> futures;
  futures.reserve(32);

  for (const auto &entry : fs::directory_iterator(dirPath)) {
    if (!entry.is_directory()) {
      continue;
    }

    futures.emplace_back(
        std::async(std::launch::async, [path = entry.path(), type]() {
          return checkRepository(path, type);
        }));

    if (futures.size() >= maxThreads) {
      results.push_back(futures.front().get());
      futures.erase(futures.begin());
    }
  }

  for (auto &future : futures) {
    results.push_back(future.get());
  }

  return results;
}

/**
 * Print results in a formatted table
 *
 * @param results The vector of repository statuses
 * @param reportStream Optional output file stream for the report
 */
void printResults(const std::vector<RepoStatus> &results,
                  std::ofstream *reportStream = nullptr) {
  if (results.empty()) {
    return;
  }

  std::ostringstream oss;
  oss << "\n" << std::string(100, '=') << "\n";
  oss << std::left << std::setw(30) << "Name" << std::setw(12) << "Type"
      << std::setw(15) << "Branch" << std::setw(10) << "Behind" << std::setw(14)
      << "Uncommitted"
      << "Status\n";
  oss << std::string(100, '-') << "\n";

  for (const auto &status : results) {
    oss << std::left << std::setw(30) << status.name << std::setw(12)
        << status.type << std::setw(15)
        << (status.currentBranch.empty() ? "N/A" : status.currentBranch);

    if (!status.isRepo) {
      oss << std::setw(10) << "N/A" << std::setw(14) << "N/A"
          << "⚠️  Not a git repo\n";
    } else if (!status.error.empty()) {
      oss << std::setw(10) << "N/A" << std::setw(14) << "N/A"
          << "⚠️  " << status.error << "\n";
    } else if (status.pulled) {
      oss << std::setw(10) << "0" << std::setw(14)
          << (status.hadUncommittedChanges ? "Yes" : "No");
      if (status.hadUncommittedChanges) {
        oss << "✅ Pulled (⚠️  had uncommitted changes)\n";
      } else {
        oss << "✅ Pulled and up to date\n";
      }
    } else if (!status.pullError.empty()) {
      oss << std::setw(10) << status.behindBy << std::setw(14)
          << (status.hadUncommittedChanges ? "Yes" : "No")
          << "❌ Pull failed: " << status.pullError << "\n";
    } else if (status.hasUpdates) {
      oss << std::setw(10) << status.behindBy << std::setw(14)
          << (status.hadUncommittedChanges ? "Yes" : "No")
          << "🔴 Updates available\n";
    } else {
      oss << std::setw(10) << "0" << std::setw(14)
          << (status.hadUncommittedChanges ? "Yes" : "No") << "✅ Up to date\n";
    }
  }

  oss << std::string(100, '=') << "\n";
  writeOutput(oss.str(), reportStream);
}

/**
 * Print results section with fallback message
 *
 * @param title The section title
 * @param results The vector of repository statuses
 * @param reportStream Optional output file stream for the report
 */
void printResultsSection(const std::string &title,
                         const std::vector<RepoStatus> &results,
                         std::ofstream *reportStream = nullptr) {
  std::ostringstream oss;
  oss << "\n" << title << ":\n";
  writeOutput(oss.str(), reportStream);

  if (!results.empty()) {
    printResults(results, reportStream);
  } else {
    std::ostringstream msg;
    if (title == "EXTENSIONS") {
      msg << "No extensions found or extensions directory doesn't exist.\n";
    } else if (title == "SKINS") {
      msg << "No skins found or skins directory doesn't exist.\n";
    }
    writeOutput(msg.str(), reportStream);
  }
}

/**
 * Update a specific extension or skin
 *
 * @param basePath The base MediaWiki installation path
 * @param type The type of repository (core, extension, skin)
 * @param name The name of the extension or skin (empty for core)
 * @return 0 on success, 1 on error
 */
int updateSingleRepo(const fs::path &basePath, const std::string &type,
                     const std::string &name) {
  fs::path repoPath;
  std::string displayName;

  if (type == "core") {
    repoPath = basePath;
    displayName = "MediaWiki core";
  } else if (type == "extension") {
    repoPath = basePath / "extensions" / name;
    displayName = "extension '" + name + "'";
  } else if (type == "skin") {
    repoPath = basePath / "skins" / name;
    displayName = "skin '" + name + "'";
  } else {
    std::cerr << "Error: Invalid type '" << type
              << "'. Must be 'core', 'extension', or 'skin'.\n";
    return 1;
  }

  if (!fs::exists(repoPath)) {
    std::cerr << "Error: " << displayName
              << " not found at: " << repoPath.string() << "\n";
    return 1;
  }

  if (!fs::is_directory(repoPath)) {
    std::cerr << "Error: Path exists but is not a directory: "
              << repoPath.string() << "\n";
    return 1;
  }

  std::cout << "Checking " << displayName << " at: " << repoPath.string()
            << "\n";

  RepoStatus status = checkRepository(repoPath, type);

  if (!status.isRepo) {
    std::cerr << "Error: Not a git repository\n";
    return 1;
  }

  if (!status.error.empty()) {
    std::cerr << "Error: " << status.error << "\n";
    return 1;
  }

  std::cout << "\nRepository Status:\n";
  std::cout << "  Branch: " << status.currentBranch << "\n";
  std::cout << "  Uncommitted changes: "
            << (status.hadUncommittedChanges ? "Yes" : "No") << "\n";
  std::cout << "  Commits behind: "
            << (status.behindBy >= 0 ? std::to_string(status.behindBy)
                                     : "Unknown")
            << "\n";

  if (status.behindBy <= 0) {
    std::cout << "\n✅ Already up to date!\n";
    return 0;
  }

  // Prompt user
  std::ostringstream prompt;
  prompt << "\nPull " << status.behindBy << " commit"
         << (status.behindBy > 1 ? "s" : "") << "?";
  if (status.hadUncommittedChanges) {
    prompt << "\n  ⚠️  WARNING: Repository has uncommitted changes!";
  }
  prompt << "\n  ";

  bool confirmed = g_autoYes || promptForConfirmation(prompt.str());

  if (!confirmed) {
    std::cout << "Update cancelled.\n";
    return 0;
  }

  std::cout << "Pulling updates...\n";
  std::string pullError;
  if (performGitPull(repoPath, pullError)) {
    std::cout << "\n✅ Successfully updated!\n";
    return 0;
  } else {
    std::cerr << "\n❌ Pull failed:\n" << pullError << "\n";
    return 1;
  }
}

/**
 * Main function
 *
 * @param argc Argument count
 * @param argv Argument vector
 * @return Exit code
 */
int main(int argc, char *argv[]) {
  std::string mwPath;

  // Parse command-line arguments
  for (int i = 1; i < argc; i++) {
    std::string arg = argv[i];
    if (arg == "-v" || arg == "--verbose") {
      g_verbose = true;
      std::cout << "Verbose mode enabled\n";
    } else if (arg == "--report-only") {
      g_reportOnly = true;
      std::cout << "Report-only mode enabled (no automatic pulls)\n";
    } else if (arg == "--yes" || arg == "-y") {
      g_autoYes = true;
      std::cout << "Auto-yes mode enabled (no prompts)\n";
    } else if (arg == "--report-file") {
      if (i + 1 >= argc) {
        std::cerr << "Error: --report-file requires a filename argument\n";
        return 1;
      }
      g_reportFile = argv[++i];
      std::cout << "Report will be saved to: " << g_reportFile << "\n";
    } else if (arg == "--timeout") {
      if (i + 1 >= argc) {
        std::cerr << "Error: --timeout requires SECONDS argument\n";
        return 1;
      }
      try {
        g_commandTimeoutSeconds = std::stoi(argv[++i]);
      } catch (...) {
        std::cerr << "Error: Invalid timeout value\n";
        return 1;
      }
      if (g_commandTimeoutSeconds < 1) {
        std::cerr << "Error: --timeout must be >= 1\n";
        return 1;
      }
      std::cout << "Command timeout set to: " << g_commandTimeoutSeconds
                << "s\n";
    } else if (arg == "--pull-timeout") {
      if (i + 1 >= argc) {
        std::cerr << "Error: --pull-timeout requires SECONDS argument\n";
        return 1;
      }
      try {
        g_pullTimeoutSeconds = std::stoi(argv[++i]);
      } catch (...) {
        std::cerr << "Error: Invalid pull-timeout value\n";
        return 1;
      }
      if (g_pullTimeoutSeconds < 1) {
        std::cerr << "Error: --pull-timeout must be >= 1\n";
        return 1;
      }
      std::cout << "Pull timeout set to: " << g_pullTimeoutSeconds << "s\n";
    } else if (arg == "--gerrit-delay") {
      if (i + 1 >= argc) {
        std::cerr << "Error: --gerrit-delay requires SECONDS argument\n";
        return 1;
      }
      try {
        g_gerritDelaySeconds = std::stoi(argv[++i]);
      } catch (...) {
        std::cerr << "Error: Invalid gerrit-delay value\n";
        return 1;
      }
      if (g_gerritDelaySeconds < 0) {
        std::cerr << "Error: --gerrit-delay must be >= 0\n";
        return 1;
      }
      std::cout << "Gerrit delay set to: " << g_gerritDelaySeconds << "s\n";
    } else if (arg == "--update") {
      if (i + 1 >= argc) {
        std::cerr << "Error: --update requires TYPE argument\n";
        std::cerr << "Usage: --update <core|extension|skin> [name]\n";
        return 1;
      }
      g_updateMode = true;
      g_updateType = argv[++i];
      // For core, name is not required
      if (g_updateType == "core") {
        g_updateName = "";
      } else if (i + 1 >= argc) {
        std::cerr << "Error: --update " << g_updateType
                  << " requires NAME argument\n";
        std::cerr << "Usage: --update <extension|skin> <name>\n";
        return 1;
      } else {
        g_updateName = argv[++i];
      }
    } else if (arg == "--fox") {
      std::cout << "look at them!!  -->  🦊\n";
      return 0;
    } else if (arg == "-h" || arg == "--help") {
      std::cout << "Usage: " << argv[0] << " [OPTIONS] [PATH]\n\n";
      std::cout << "Check MediaWiki extensions and skins for updates\n";
      std::cout << "and update them if needed.\n\n";
      std::cout << "Options:\n";
      std::cout << "  -v, --verbose      Enable verbose output\n";
      std::cout
          << "  --report-only      Only report status, don't pull updates\n";
      std::cout << "  -y, --yes          Auto-confirm all pull prompts\n";
      std::cout << "  --report-file FILE Save results and summary to a file\n";
      std::cout << "  --timeout SEC      Timeout for git checks/fetch in "
                   "seconds\n";
      std::cout << "                     (default: 30)\n";
      std::cout << "  --pull-timeout SEC Timeout for git pull in seconds\n";
      std::cout << "                     (default: 120)\n";
      std::cout << "  --gerrit-delay SEC Delay between Gerrit interactions\n";
      std::cout << "                     (default: 1, 0 disables)\n";
      std::cout << "  --update TYPE NAME Update a specific extension or skin\n";
      std::cout << "                     TYPE must be 'core', 'extension', or "
                   "'skin'\n";
      std::cout << "                     NAME required for extension/skin\n";
      std::cout << "                     Examples:\n";
      std::cout << "                       --update core\n";
      std::cout
          << "                       --update extension WikimediaEvents\n";
      std::cout << "                       --update skin Vector\n";
      std::cout << "  -h, --help         Show this help message\n";
      std::cout << "  --version          Show version number\n\n";
      std::cout << "Arguments:\n";
      std::cout << "  PATH               Path to MediaWiki installation\n";
      std::cout << "                     (if not provided, will prompt)\n\n";
      std::cout << "Note: Repositories on master/main branches with updates\n";
      std::cout << "      will be prompted for pull unless --yes is used.\n";
      std::cout << "      Use --report-only to skip pulling entirely.\n";
      std::cout
          << "      A warning will be shown if uncommitted changes exist.\n\n";
      std::cout << "Version: " << VERSION << "\n";
      std::cout << "Source: " << SOURCE_URL << "\n";
      return 0;
    } else if (arg == "--version") {
      std::cout << "local_mw\n";
      std::cout << "Version: " << VERSION << "\n";
      std::cout << "Source: " << SOURCE_URL << "\n";
      return 0;
    } else if (arg[0] == '-') {
      std::cerr << "Error: Unknown option '" << arg << "'\n";
      std::cerr << "Use --help for usage information.\n";
      return 1;
    } else if (mwPath.empty()) {
      mwPath = arg;
    } else {
      std::cerr << "Error: Unexpected argument '" << arg << "'\n";
      std::cerr << "MediaWiki path already specified as: " << mwPath << "\n";
      return 1;
    }
  }

  // Get MediaWiki installation path if not provided
  if (mwPath.empty()) {
    std::cout << "Enter MediaWiki installation path: ";
    std::getline(std::cin, mwPath);
  }

  fs::path basePath(mwPath);

  if (!fs::exists(basePath) || !fs::is_directory(basePath)) {
    std::cerr << "Error: Invalid MediaWiki installation path: " << mwPath
              << "\n";
    return 1;
  }

  // Validate that the directory is a MediaWiki installation
  if (!isMediaWikiDirectory(basePath)) {
    std::cerr
        << "Error: Directory does not appear to be a MediaWiki installation.\n";
    std::cerr << "Expected files/directories not found (index.php, api.php, "
                 "includes/, extensions/, skins/).\n";
    return 1;
  }

  // Handle single repository update mode
  if (g_updateMode) {
    return updateSingleRepo(basePath, g_updateType, g_updateName);
  }

  std::cout << "Checking MediaWiki installation at: " << basePath.string()
            << "\n";
  if (!g_reportOnly) {
    std::cout << "Auto-pull enabled for master/main branches with updates\n";
  }
  std::cout << "This may take a moment...\n";

  // Check MediaWiki core (the base installation path)
  std::cout << "Checking MediaWiki core...\n";
  std::vector<RepoStatus> coreResults;
  coreResults.push_back(checkRepository(basePath, "core"));

  // Check extensions
  fs::path extensionsPath = basePath / "extensions";
  std::cout << "Checking extensions (" << countDirectories(extensionsPath)
            << ")...\n";
  printVerboseDirectoryHeader("extensions", extensionsPath);
  std::vector<RepoStatus> extensionResults =
      scanDirectory(extensionsPath, "extension");

  // Check skins
  fs::path skinsPath = basePath / "skins";
  std::cout << "Checking skins (" << countDirectories(skinsPath) << ")...\n";
  printVerboseDirectoryHeader("skins", skinsPath);
  std::vector<RepoStatus> skinResults = scanDirectory(skinsPath, "skin");

  // Open report file if specified
  std::ofstream reportFile;
  if (!g_reportFile.empty()) {
    reportFile.open(g_reportFile);
    if (!reportFile.is_open()) {
      std::cerr << "Warning: Could not open report file: " << g_reportFile
                << "\n";
    } else {
      // Write timestamp at the top of the report
      std::time_t now = std::time(nullptr);
      std::tm *localTime = std::localtime(&now);
      char timeBuffer[20];
      std::strftime(timeBuffer, sizeof(timeBuffer), "%Y-%m-%d %H:%M:%S",
                    localTime);
      reportFile << timeBuffer << "\n";
    }
  }
  std::ofstream *reportStream = (reportFile.is_open()) ? &reportFile : nullptr;

  // Print results
  if (!coreResults.empty()) {
    writeOutput("\nMEDIAWIKI CORE:\n", reportStream);
    printResults(coreResults, reportStream);
  }

  printResultsSection("EXTENSIONS", extensionResults, reportStream);
  printResultsSection("SKINS", skinResults, reportStream);

  // Summary
  Statistics coreStats = calculateStats(coreResults);
  Statistics extensionStats = calculateStats(extensionResults);
  Statistics skinStats = calculateStats(skinResults);

  int totalRepos = static_cast<int>(
      coreResults.size() + extensionResults.size() + skinResults.size());
  int upToDate =
      coreStats.upToDate + extensionStats.upToDate + skinStats.upToDate;
  int hasUpdates =
      coreStats.hasUpdates + extensionStats.hasUpdates + skinStats.hasUpdates;
  int errors = coreStats.errors + extensionStats.errors + skinStats.errors;

  std::ostringstream summary;
  summary << "\nSUMMARY:\n";
  summary << "  Total repositories: " << totalRepos << "\n";
  summary << "  Up to date: " << upToDate << "\n";
  summary << "  Updates available: " << hasUpdates << "\n";
  summary << "  Errors/Warnings: " << errors << "\n\n";
  writeOutput(summary.str(), reportStream);

  if (reportFile.is_open()) {
    reportFile.close();
    std::cout << "Report saved to: " << g_reportFile << "\n";
  }

  return 0;
}
