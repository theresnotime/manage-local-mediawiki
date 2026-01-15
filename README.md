# local_mw
Yet another tool to manage local MediaWiki installations.

## Building
Installs to `~/.local/bin` by default (configurable via PREFIX variable). I'm working on making releases with prebuilt binaries :3

### Requirements
- C++17 compatible compiler (g++ or clang++)
- Make (for using the Makefile)
- Git (for version detection and runtime functionality)
- Unix-like OS (macOS, Linux, BSD, etc.)

### Build steps
```bash
git clone https://github.com/theresnotime/manage-local-mediawiki.git && \
cd manage-local-mediawiki && \
make install
```

## --help output
```
~ $ local_mw --help
Usage: local_mw [OPTIONS] [PATH]

Check MediaWiki extensions and skins for updates
and update them if needed.

Options:
  -v, --verbose      Enable verbose output
  --report-only      Only report status, don't pull updates
  -y, --yes          Auto-confirm all pull prompts
  --report-file FILE Save results and summary to a file
  --update TYPE NAME Update a specific extension or skin
                     TYPE must be 'core', 'extension', or 'skin'
                     NAME required for extension/skin
                     Examples:
                       --update core
                       --update extension WikimediaEvents
                       --update skin Vector
  -h, --help         Show this help message
  --version          Show version number

Arguments:
  PATH               Path to MediaWiki installation
                     (if not provided, will prompt)

Note: Repositories on master/main branches with updates
      will be prompted for pull unless --yes is used.
      Use --report-only to skip pulling entirely.
      A warning will be shown if uncommitted changes exist.
```

## Example usage
### Getting a report without making changes
```
~ $ local_mw --report-only ./test-mw
Report-only mode enabled (no automatic pulls)
Checking MediaWiki installation at: ./test-mw
This may take a moment...
Checking MediaWiki core...
Checking extensions (2)...
Checking skins (1)...

MEDIAWIKI CORE:

====================================================================================================
Name                          Type        Branch         Behind    Uncommitted   Status
----------------------------------------------------------------------------------------------------
test-mw                       core        master         1         No            🔴 Updates available
====================================================================================================

EXTENSIONS:

====================================================================================================
Name                          Type        Branch         Behind    Uncommitted   Status
----------------------------------------------------------------------------------------------------
WikimediaEvents               extension   master         0         No            ✅ Up to date
WikimediaMessages             extension   master         0         No            ✅ Up to date
====================================================================================================

SKINS:

====================================================================================================
Name                          Type        Branch         Behind    Uncommitted   Status
----------------------------------------------------------------------------------------------------
Vector                        skin        master         249       No            🔴 Updates available
====================================================================================================

SUMMARY:
  Total repositories: 4
  Up to date: 2
  Updates available: 2
  Errors/Warnings: 0
```

### Updating repos
I realise now that it should report first, and *then* prompt to pull, rather than prompting during the report. Will fix later :3
```
~ $ local_mw ./test-mw
Checking MediaWiki installation at: ./test-mw
Auto-pull enabled for master/main branches with updates
This may take a moment...
Checking MediaWiki core...

Pull updates for 'test-mw' (core, 1 commit behind)
   [y/N]: y
Checking extensions (2)...
Checking skins (1)...

Pull updates for 'Vector' (skin, 249 commits behind)
   [y/N]: y

MEDIAWIKI CORE:

====================================================================================================
Name                          Type        Branch         Behind    Uncommitted   Status
----------------------------------------------------------------------------------------------------
test-mw                       core        master         0         No            ✅ Pulled and up to date
====================================================================================================

EXTENSIONS:

====================================================================================================
Name                          Type        Branch         Behind    Uncommitted   Status
----------------------------------------------------------------------------------------------------
WikimediaEvents               extension   master         0         No            ✅ Up to date
WikimediaMessages             extension   master         0         No            ✅ Up to date
====================================================================================================

SKINS:

====================================================================================================
Name                          Type        Branch         Behind    Uncommitted   Status
----------------------------------------------------------------------------------------------------
Vector                        skin        master         0         No            ✅ Pulled and up to date
====================================================================================================

SUMMARY:
  Total repositories: 4
  Up to date: 2
  Updates available: 2
  Errors/Warnings: 0
```

## TODO
- [ ] Improve reporting and pulling flow (report first, then prompt to pull)
- [ ] Make releases with prebuilt binaries
- [ ] Add "no colour/emoji" option for plain text environments
- [ ] Add tests :D
- [ ] Do composer update/npm update where applicable (*that'll be fun..*)