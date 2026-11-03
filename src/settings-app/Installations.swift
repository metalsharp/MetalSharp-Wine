// Where WineMetalGL is installed, switching it on and off there, and
// installing it: CrossOver bottles, MetalSharp's prefixes (its Steam and GOG
// ones), the prefixes of Steam games NotProton runs (issue #73), ~/.wine, and
// the prefixes installed into from here. On and off is the prefix's DLL
// override, opengl32=native (WineMetalGL) or builtin (Wine's own OpenGL),
// set through that Wine's `reg`. Installing runs the release's own scripts,
// which the app carries (Contents/Resources/Installer: install-crossover.sh,
// install-wine-module.sh, and the release's module directory when the app was
// built with it; without it, the scripts download the latest release).

import AppKit
import SwiftUI

struct WineTarget: Identifiable, Hashable, Sendable {
    enum Kind { case crossover, prefix, notproton }
    var kind: Kind
    var name: String
    var prefix: URL      // the bottle's directory, or the WINEPREFIX
    var wine: URL?       // a prefix's Wine (the directory with bin/wine); CrossOver's own for a bottle;
                         // for a NotProton game, its runner (a copy of CrossOver)
    var installed: Bool  // winemetalgl.dll is in its system32
    var enabled: Bool    // opengl32=native
    var id: String { prefix.path }
}

/// A Wine on this Mac, for Install: its name and the directory with bin/wine.
struct FoundWine: Identifiable, Hashable, Sendable {
    var name: String
    var wine: URL
    var id: String { wine.path }
}

@MainActor
final class Installations: ObservableObject {
    @Published private(set) var targets: [WineTarget] = []
    @Published private(set) var wines: [FoundWine] = []  // Install's choices (findWines)
    @Published private(set) var crossovers: [URL] = []   // CrossOver copies (findCrossOvers), each its Contents/SharedSupport/CrossOver
    @Published private(set) var busy: String?
    @Published var report: Report?

    struct Report: Identifiable {
        let id = UUID()
        let title: String
        let text: String
    }

    private let home = AppEnvironment.home
    private let savedKey = "installedPrefixes"  // [[prefix, wine]] installed into from here
    private let crossoverKey = "crossover"      // the CrossOver last installed with (its SharedSupport/CrossOver)
    private let bottlesKey = "installedBottles"  // bottles installed into from here, wherever they are

    /// The CrossOver the bottles run in: the one last installed with, else the first found.
    var crossover: URL? {
        if let saved = AppEnvironment.defaults.string(forKey: crossoverKey),
           Self.isCrossOver(URL(fileURLWithPath: saved)) { return URL(fileURLWithPath: saved) }
        return crossovers.first
    }

    nonisolated static func isCrossOver(_ dir: URL) -> Bool {
        FileManager.default.isExecutableFile(atPath: dir.appendingPathComponent("bin/wine").path)
    }

    /// A CrossOver app's (or its Contents/SharedSupport/CrossOver's) Wine directory.
    nonisolated static func crossoverDir(_ url: URL) -> URL {
        url.pathExtension == "app" ? url.appendingPathComponent("Contents/SharedSupport/CrossOver") : url
    }

    /// The CrossOver.app a Contents/SharedSupport/CrossOver belongs to, for its name in a menu.
    static func crossoverApp(_ dir: URL) -> URL {
        dir.deletingLastPathComponent().deletingLastPathComponent().deletingLastPathComponent()
    }

    /// Every CrossOver bottle on this Mac (its directory, with cxbottle.conf):
    /// CrossOver's own Bottles folder, the bottles installed into from here,
    /// and any other Spotlight knows, so a bottle shows even without CrossOver.app.
    nonisolated static func findBottles(home: URL, saved: [String]) -> [URL] {
        let fm = FileManager.default
        var found: [URL] = []
        func add(_ dir: URL) {
            let real = dir.resolvingSymlinksInPath()
            guard fm.fileExists(atPath: real.appendingPathComponent("cxbottle.conf").path),
                  fm.fileExists(atPath: real.appendingPathComponent("drive_c").path),
                  !found.contains(where: { $0.path == real.path }) else { return }
            found.append(real)
        }
        let bottlesDir = home.appendingPathComponent("Library/Application Support/CrossOver/Bottles")
        for bottle in (try? fm.contentsOfDirectory(at: bottlesDir, includingPropertiesForKeys: nil)) ?? [] { add(bottle) }
        for path in saved { add(URL(fileURLWithPath: path)) }
        let (status, output) = executeNow(URL(fileURLWithPath: "/usr/bin/mdfind"), ["kMDItemFSName == 'cxbottle.conf'"])
        if status == 0 {
            for line in output.split(separator: "\n") where line.hasSuffix("/cxbottle.conf") {
                add(URL(fileURLWithPath: String(line)).deletingLastPathComponent())
            }
        }
        return found
    }

    /// Every copy of CrossOver: in /Applications and ~/Applications, the one
    /// last installed with, and any other Spotlight knows (by its bundle ID).
    nonisolated static func findCrossOvers(home: URL, saved: String?) -> [URL] {
        var found: [URL] = []
        func add(_ app: URL) {
            let dir = crossoverDir(app).resolvingSymlinksInPath()
            if isCrossOver(dir), !found.contains(where: { $0.path == dir.path }) { found.append(dir) }
        }
        add(URL(fileURLWithPath: "/Applications/CrossOver.app"))
        add(home.appendingPathComponent("Applications/CrossOver.app"))
        if let saved { add(URL(fileURLWithPath: saved)) }
        let (status, output) = executeNow(URL(fileURLWithPath: "/usr/bin/mdfind"), ["kMDItemCFBundleIdentifier == 'com.codeweavers.CrossOver'"])
        if status == 0 {
            for line in output.split(separator: "\n") where line.hasSuffix(".app") { add(URL(fileURLWithPath: String(line))) }
        }
        return found
    }

    /// The release carried with the scripts (else they download it).
    var carriesRelease: Bool {
        guard let installer else { return false }
        return FileManager.default.fileExists(atPath: installer.appendingPathComponent("module").path)
    }

    /// The install scripts the app carries.
    var installer: URL? {
        guard let dir = Bundle.main.resourceURL?.appendingPathComponent("Installer"),
              FileManager.default.fileExists(atPath: dir.appendingPathComponent("install-wine-module.sh").path)
        else { return nil }
        return dir
    }

    var bottles: [WineTarget] { targets.filter { $0.kind == .crossover } }
    var anyInstalled: Bool { targets.contains { $0.installed } }

    init() { refresh() }

    // MARK: Finding them

    func refresh() {
        let fm = FileManager.default
        var found: [WineTarget] = []
        // The bottles known at once; the rest (Spotlight's) follow below.
        let savedBottles = AppEnvironment.defaults.stringArray(forKey: bottlesKey) ?? []
        let bottlesDir = home.appendingPathComponent("Library/Application Support/CrossOver/Bottles")
        for bottle in ((try? fm.contentsOfDirectory(at: bottlesDir, includingPropertiesForKeys: nil)) ?? []) + savedBottles.map({ URL(fileURLWithPath: $0) })
        where fm.fileExists(atPath: bottle.appendingPathComponent("cxbottle.conf").path) && !found.contains(where: { $0.prefix.path == bottle.path }) {
            found.append(target(.crossover, name: bottle.lastPathComponent, prefix: bottle, wine: crossover))
        }
        let metalsharp = home.appendingPathComponent(".metalsharp")
        let metalsharpWine = metalsharp.appendingPathComponent("runtime/wine")
        for dir in (try? fm.contentsOfDirectory(at: metalsharp, includingPropertiesForKeys: nil)) ?? []
        where dir.lastPathComponent.hasPrefix("prefix-") && fm.fileExists(atPath: dir.appendingPathComponent("drive_c").path) {
            let name = "MetalSharp " + dir.lastPathComponent.dropFirst("prefix-".count)
            found.append(target(.prefix, name: name, prefix: dir, wine: metalsharpWine))
        }
        // MetalSharp's GOG games run in bottles/gog-prefix/prefix.
        let bottlesPrefixes = metalsharp.appendingPathComponent("bottles")
        for dir in (try? fm.contentsOfDirectory(at: bottlesPrefixes, includingPropertiesForKeys: nil)) ?? []
        where fm.fileExists(atPath: dir.appendingPathComponent("prefix/drive_c").path) {
            found.append(target(.prefix, name: "MetalSharp " + dir.lastPathComponent,
                                prefix: dir.appendingPathComponent("prefix"), wine: metalsharpWine))
        }
        for game in Self.findNotProtonGames(home: home) {
            found.append(target(.notproton, name: game.name, prefix: game.prefix, wine: game.runner))
        }
        let defaultPrefix = home.appendingPathComponent(".wine")
        if fm.fileExists(atPath: defaultPrefix.appendingPathComponent("drive_c").path) {
            found.append(target(.prefix, name: "Default prefix", prefix: defaultPrefix, wine: wines.first?.wine))
        }
        for pair in AppEnvironment.defaults.array(forKey: savedKey) as? [[String]] ?? [] where pair.count == 2 {
            let prefix = URL(fileURLWithPath: pair[0])
            guard !found.contains(where: { $0.prefix.path == prefix.path }),
                  fm.fileExists(atPath: prefix.appendingPathComponent("drive_c").path) else { continue }
            found.append(target(.prefix, name: prefix.lastPathComponent, prefix: prefix, wine: URL(fileURLWithPath: pair[1])))
        }
        targets = found
        // The Wines on this Mac, looked for away from the main thread (Spotlight
        // among them); ~/.wine's Wine is the first, once they are known.
        let home = self.home, saved = AppEnvironment.defaults.string(forKey: crossoverKey)
        Task {
            let wines = await Task.detached { Self.findWines(home: home) }.value
            self.crossovers = await Task.detached { Self.findCrossOvers(home: home, saved: saved) }.value
            let bottles = await Task.detached { Self.findBottles(home: home, saved: savedBottles) }.value
            let bottleCount = self.targets.filter { $0.kind == .crossover }.count
            for bottle in bottles where !self.targets.contains(where: { $0.prefix.resolvingSymlinksInPath().path == bottle.path }) {
                self.targets.insert(self.target(.crossover, name: bottle.lastPathComponent, prefix: bottle, wine: nil), at: bottleCount)
            }
            for i in self.targets.indices where self.targets[i].kind == .crossover { self.targets[i].wine = self.crossover }
            self.wines = wines
            if let i = self.targets.firstIndex(where: { $0.prefix.path == defaultPrefix.path && $0.wine == nil }) {
                self.targets[i].wine = wines.first?.wine
            }
        }
    }

    private func target(_ kind: WineTarget.Kind, name: String, prefix: URL, wine: URL?) -> WineTarget {
        let system32 = prefix.appendingPathComponent("drive_c/windows/system32/winemetalgl.dll")
        return WineTarget(kind: kind, name: name, prefix: prefix, wine: wine,
                          installed: FileManager.default.fileExists(atPath: system32.path),
                          enabled: Self.overrideIsNative(prefix: prefix))
    }

    /// Every Wine on this Mac (the directories with bin/wine), for Install:
    /// apps that carry one (WineHQ's first) in /Applications and
    /// ~/Applications, a folder deep; Homebrew's and MacPorts'; Whisky's and
    /// MetalSharp's; and any other bin/wine Spotlight knows. CrossOver's is
    /// left out (its bottles have their own install).
    nonisolated static func findWines(home: URL) -> [FoundWine] {
        let fm = FileManager.default
        var found: [FoundWine] = []
        func add(_ name: String, _ dir: URL) {
            let real = dir.resolvingSymlinksInPath()
            guard fm.isExecutableFile(atPath: real.appendingPathComponent("bin/wine").path),
                  !real.path.contains("/CrossOver.app/"),
                  !found.contains(where: { $0.wine.path == real.path }) else { return }
            found.append(FoundWine(name: name, wine: real))
        }
        var apps: [URL] = []
        for dir in [URL(fileURLWithPath: "/Applications"), home.appendingPathComponent("Applications")] {
            for item in (try? fm.contentsOfDirectory(at: dir, includingPropertiesForKeys: nil)) ?? [] {
                if item.pathExtension == "app" {
                    apps.append(item)
                } else {
                    apps += ((try? fm.contentsOfDirectory(at: item, includingPropertiesForKeys: nil)) ?? []).filter { $0.pathExtension == "app" }
                }
            }
        }
        let winehq = ["Wine Stable", "Wine Devel", "Wine Staging"]
        func rank(_ app: URL) -> (Int, String) {
            let name = app.deletingPathExtension().lastPathComponent
            return (winehq.firstIndex(of: name) ?? winehq.count, name)
        }
        for app in apps.sorted(by: { rank($0) < rank($1) }) {
            for inner in ["Contents/Resources/wine", "Contents/SharedSupport/wine", "Contents/Resources"] {
                add(app.deletingPathExtension().lastPathComponent, app.appendingPathComponent(inner))
            }
        }
        for (name, bin) in [("Homebrew's Wine", "/opt/homebrew/bin/wine"), ("Homebrew's Wine", "/usr/local/bin/wine"),
                            ("MacPorts' Wine", "/opt/local/bin/wine")] {
            add(name, URL(fileURLWithPath: bin).resolvingSymlinksInPath().deletingLastPathComponent().deletingLastPathComponent())
        }
        add("Whisky's Wine", home.appendingPathComponent("Library/Application Support/com.isaacmarovitz.Whisky/Libraries/Wine"))
        add("MetalSharp's Wine", home.appendingPathComponent(".metalsharp/runtime/wine"))
        let (status, output) = executeNow(URL(fileURLWithPath: "/usr/bin/mdfind"), ["kMDItemFSName == 'wine'"])
        if status == 0 {
            // Not a CI runner's work (its builds come and go); a Wine outside an app by its path.
            for line in output.split(separator: "\n") where line.hasSuffix("/bin/wine") && !line.contains("/actions-runner/") {
                let dir = URL(fileURLWithPath: String(line)).deletingLastPathComponent().deletingLastPathComponent()
                let app = dir.pathComponents.first { $0.hasSuffix(".app") }
                add(app.map { ($0 as NSString).deletingPathExtension } ?? (dir.path as NSString).abbreviatingWithTildeInPath, dir)
            }
        }
        return found
    }

    /// A Steam game NotProton runs: its prefix (Steam's compatdata/<appid>/pfx)
    /// and the runner it last ran with.
    struct NotProtonGame: Sendable {
        var name: String
        var prefix: URL
        var runner: URL  // the runner's CrossOver directory, with lib/wine
    }

    /// The Steam games NotProton has set up (github.com/NotProtonNot/NotProton):
    /// in every Steam library, each compatdata/<appid> with a pfx and the
    /// notproton-build NotProton writes there (the runner build, then its
    /// name), whose runner, ~/Library/Application Support/notproton/runners/
    /// crossover-<build>/CrossOver, is still there. Named after the game's
    /// appmanifest.
    nonisolated static func findNotProtonGames(home: URL) -> [NotProtonGame] {
        let fm = FileManager.default
        let steam = home.appendingPathComponent("Library/Application Support/Steam")
        let runners = home.appendingPathComponent("Library/Application Support/notproton/runners")
        guard fm.fileExists(atPath: runners.path) else { return [] }
        var libraries = [steam]
        if let vdf = try? String(contentsOf: steam.appendingPathComponent("steamapps/libraryfolders.vdf"), encoding: .utf8) {
            for line in vdf.split(separator: "\n") {
                // "path"		"/Volumes/Games/SteamLibrary"
                let fields = line.split(separator: "\"", omittingEmptySubsequences: false)
                guard fields.count >= 4, fields[1] == "path", !fields[3].isEmpty else { continue }
                let library = URL(fileURLWithPath: String(fields[3]).replacingOccurrences(of: "\\\\", with: "\\"))
                if !libraries.contains(where: { $0.standardizedFileURL.path == library.standardizedFileURL.path }) {
                    libraries.append(library)
                }
            }
        }
        var games: [NotProtonGame] = []
        for library in libraries {
            let steamapps = library.appendingPathComponent("steamapps")
            let compatdata = steamapps.appendingPathComponent("compatdata")
            let apps = (try? fm.contentsOfDirectory(at: compatdata, includingPropertiesForKeys: nil)) ?? []
            for app in apps.sorted(by: { $0.lastPathComponent < $1.lastPathComponent }) {
                let prefix = app.appendingPathComponent("pfx")
                guard fm.fileExists(atPath: prefix.appendingPathComponent("drive_c").path),
                      let record = try? String(contentsOf: app.appendingPathComponent("notproton-build"), encoding: .utf8)
                else { continue }
                let lines = record.split(separator: "\n").map { $0.trimmingCharacters(in: .whitespaces) }
                guard let build = lines.first, !build.isEmpty, !build.contains("/") else { continue }
                let runner = runners.appendingPathComponent("crossover-\(build)/CrossOver")
                guard fm.fileExists(atPath: runner.appendingPathComponent("lib/wine").path) else { continue }
                let appID = app.lastPathComponent
                let title = appName(appID, steamapps: steamapps) ?? "App \(appID)"
                let runnerName = lines.count > 1 && !lines[1].isEmpty ? lines[1] : build
                games.append(NotProtonGame(name: "\(title) (NotProton \(runnerName))", prefix: prefix, runner: runner))
            }
        }
        return games
    }

    /// The "name" in a Steam library's appmanifest_<appid>.acf.
    nonisolated static func appName(_ appID: String, steamapps: URL) -> String? {
        guard let text = try? String(contentsOf: steamapps.appendingPathComponent("appmanifest_\(appID).acf"), encoding: .utf8)
        else { return nil }
        for line in text.split(separator: "\n") {
            let fields = line.split(separator: "\"", omittingEmptySubsequences: false)
            if fields.count >= 4, fields[1] == "name", !fields[3].isEmpty { return String(fields[3]) }
        }
        return nil
    }

    /// The Wine loader and wineserver NotProton runs a prefix with, as its
    /// compat_run.sh picks them: the runner's arm64 ones for a prefix of the
    /// arm64 Wine (an ARM64 system32), else its x86_64 ones (Rosetta).
    nonisolated static func notProtonLoader(runner: URL, prefix: URL) -> (loader: URL, server: URL)? {
        let fm = FileManager.default
        let ntdll = prefix.appendingPathComponent("drive_c/windows/system32/ntdll.dll")
        if peMachine(ntdll) == 0xaa64 {
            let loader = runner.appendingPathComponent("lib/wine/aarch64-unix/wine.app/Contents/MacOS/wine")
            let server = runner.appendingPathComponent("bin/wineserver-arm64")
            if fm.isExecutableFile(atPath: loader.path), fm.isExecutableFile(atPath: server.path) { return (loader, server) }
        }
        let loader = runner.appendingPathComponent("lib/wine/x86_64-unix/wine")
        guard fm.isExecutableFile(atPath: loader.path) else { return nil }
        let server = ["bin/wineserver", "bin/wineserver-x86"].map { runner.appendingPathComponent($0) }
            .first { fm.isExecutableFile(atPath: $0.path) }
        return server.map { (loader, $0) }
    }

    /// A PE file's COFF machine (0xaa64 ARM64, 0x8664 x86_64, 0x14c i386).
    nonisolated static func peMachine(_ file: URL) -> UInt16? {
        guard let handle = try? FileHandle(forReadingFrom: file) else { return nil }
        defer { try? handle.close() }
        guard let dos = try? handle.read(upToCount: 64), dos.count == 64 else { return nil }
        let offset = UInt64(dos[60]) | UInt64(dos[61]) << 8 | UInt64(dos[62]) << 16 | UInt64(dos[63]) << 24
        guard (try? handle.seek(toOffset: offset + 4)) != nil, let machine = try? handle.read(upToCount: 2), machine.count == 2
        else { return nil }
        return UInt16(machine[0]) | UInt16(machine[1]) << 8
    }

    /// The prefix's opengl32 override, from user.reg (as wineserver last saved it).
    static func overrideIsNative(prefix: URL) -> Bool {
        guard let text = try? String(contentsOf: prefix.appendingPathComponent("user.reg"), encoding: .utf8) else { return false }
        var inOverrides = false
        for line in text.split(separator: "\n") {
            if line.hasPrefix("[") {
                inOverrides = line.lowercased().hasPrefix("[software\\\\wine\\\\dlloverrides]")
            } else if inOverrides, line.lowercased().hasPrefix("\"opengl32\"=") {
                return line.dropFirst("\"opengl32\"=".count).lowercased().hasPrefix("\"native")
            }
        }
        return false
    }

    // MARK: On and off

    func setEnabled(_ target: WineTarget, _ on: Bool) {
        guard let wine = target.wine else {
            report = Report(title: "No Wine for \(target.name)",
                            text: "WineMetalGL Settings does not know which Wine runs \(target.prefix.path).")
            return
        }
        let value = on ? "native" : "builtin"
        var arguments = ["reg", "add", "HKCU\\Software\\Wine\\DllOverrides", "/v", "opengl32", "/d", value, "/f"]
        var environment = ["WINEDEBUG": "-all"]
        var executable = wine.appendingPathComponent("bin/wine")
        if target.kind == .crossover {
            arguments = ["--bottle", target.prefix.path, "--wait-children"] + arguments
        } else {
            environment["WINEPREFIX"] = target.prefix.path
        }
        if target.kind == .notproton {
            guard let tools = Self.notProtonLoader(runner: wine, prefix: target.prefix) else {
                report = Report(title: "No Wine for \(target.name)", text: "\(wine.path) has no Wine loader for this prefix.")
                return
            }
            executable = tools.loader
            environment["WINESERVER"] = tools.server.path
        }
        run(on ? "Turning WineMetalGL on in \(target.name)…" : "Turning WineMetalGL off in \(target.name)…",
            executable, arguments, environment) { [weak self] ok, output in
            guard let self else { return }
            if ok {
                // user.reg follows when wineserver next saves it.
                if let i = self.targets.firstIndex(where: { $0.id == target.id }) { self.targets[i].enabled = on }
            } else {
                self.report = Report(title: "Could not switch WineMetalGL in \(target.name)", text: output)
            }
        }
    }

    // MARK: Installing

    /// Install into CrossOver: a bottle, with a copy of CrossOver (the one
    /// the bottles run in when none is given).
    func installCrossOver(_ bottle: WineTarget, using copy: URL? = nil) {
        installCrossOver(bottle: bottle.prefix, name: bottle.name, using: copy ?? crossover)
    }

    /// Install into CrossOver, choosing in Finder: a CrossOver app anywhere (a
    /// copy, or one kept outside Applications), then one of its bottles.
    func installCrossOverCustom() {
        let appPanel = NSOpenPanel()
        appPanel.message = "Choose the CrossOver app to install WineMetalGL with (any copy of CrossOver, wherever it is)."
        appPanel.prompt = "Choose CrossOver"
        appPanel.canChooseDirectories = false
        appPanel.canChooseFiles = true
        appPanel.allowedContentTypes = [.applicationBundle]
        appPanel.directoryURL = URL(fileURLWithPath: "/Applications")
        guard appPanel.runModal() == .OK, let app = appPanel.url else { return }
        let copy = Self.crossoverDir(app)
        guard Self.isCrossOver(copy) else {
            report = Report(title: "Not CrossOver", text: "\(app.path) has no Contents/SharedSupport/CrossOver/bin/wine.")
            return
        }
        chooseBottle(using: copy)
    }

    /// Install into CrossOver, choosing a bottle anywhere in Finder (with the
    /// given CrossOver, else the one the bottles run in, else none: the
    /// script then installs into the bottle only).
    func chooseBottle(using copy: URL? = nil) {
        let bottlePanel = NSOpenPanel()
        bottlePanel.message = "Choose the CrossOver bottle whose games should use WineMetalGL."
        bottlePanel.prompt = "Choose Bottle"
        bottlePanel.canChooseDirectories = true
        bottlePanel.canChooseFiles = false
        bottlePanel.showsHiddenFiles = true
        bottlePanel.directoryURL = home.appendingPathComponent("Library/Application Support/CrossOver/Bottles")
        guard bottlePanel.runModal() == .OK, let bottle = bottlePanel.url else { return }
        guard FileManager.default.fileExists(atPath: bottle.appendingPathComponent("cxbottle.conf").path) else {
            report = Report(title: "Not a CrossOver bottle", text: "\(bottle.path) has no cxbottle.conf.")
            return
        }
        installCrossOver(bottle: bottle, name: bottle.lastPathComponent, using: copy ?? crossover)
    }

    private func installCrossOver(bottle: URL, name: String, using copy: URL?) {
        guard let installer else { return }
        var arguments = [installer.appendingPathComponent("install-crossover.sh").path, bottle.path]
        if let copy { arguments.append(copy.path) }
        run("Installing WineMetalGL into \(name)…", URL(fileURLWithPath: "/bin/sh"), arguments, [:]) { [weak self] ok, output in
            guard let self else { return }
            // The bottles' switches then use that CrossOver's Wine.
            if ok, let copy { AppEnvironment.defaults.set(copy.path, forKey: self.crossoverKey) }
            if ok {
                var saved = AppEnvironment.defaults.stringArray(forKey: self.bottlesKey) ?? []
                if !saved.contains(bottle.path) { saved.append(bottle.path) }
                AppEnvironment.defaults.set(saved, forKey: self.bottlesKey)
            }
            self.finishInstall(ok, output, into: name)
        }
    }

    /// Install into a Steam game NotProton runs: WineMetalGL into its runner
    /// (lib/wine, as for any Wine) and its prefix, set up with the loader
    /// NotProton runs the prefix with.
    func installNotProton(_ target: WineTarget) {
        guard let installer, let runner = target.wine else { return }
        guard let arguments = notProtonInstall(installer, target, runner) else {
            report = Report(title: "Installing into \(target.name) failed", text: "\(runner.path) has no Wine loader for this prefix.")
            return
        }
        run("Installing WineMetalGL into \(target.name)…", URL(fileURLWithPath: "/bin/sh"), arguments.0, arguments.1) { [weak self] ok, output in
            self?.finishInstall(ok, output, into: target.name)
        }
    }

    private func notProtonInstall(_ installer: URL, _ target: WineTarget, _ runner: URL) -> ([String], [String: String])? {
        guard let tools = Self.notProtonLoader(runner: runner, prefix: target.prefix) else { return nil }
        return ([installer.appendingPathComponent("install-wine-module.sh").path, runner.path, target.prefix.path],
                ["WINEMETALGL_WINE_LOADER": tools.loader.path, "WINESERVER": tools.server.path])
    }

    /// After an update (Updater.swift): this app's release, installed again
    /// into every bottle and prefix that has WineMetalGL, the way it was
    /// installed there.
    func updateEverywhere(to version: String) async {
        refresh()
        guard let installer else { return }
        var failed: [String] = []
        var updated: [String] = []
        for target in targets where target.installed {
            var arguments: [String]
            var environment: [String: String] = [:]
            if target.kind == .notproton, let runner = target.wine, let install = notProtonInstall(installer, target, runner) {
                (arguments, environment) = install
            } else if target.kind == .notproton {
                failed.append("\(target.name): its NotProton runner has no Wine loader for it.")
                continue
            } else if target.kind == .crossover {
                arguments = [installer.appendingPathComponent("install-crossover.sh").path, target.prefix.path] + (crossover.map { [$0.path] } ?? [])
            } else if let wine = target.wine {
                arguments = [installer.appendingPathComponent("install-wine-module.sh").path, wine.path, target.prefix.path]
            } else {
                failed.append("\(target.name): WineMetalGL Settings does not know which Wine runs it.")
                continue
            }
            let (ok, output) = await perform("Updating WineMetalGL in \(target.name)…", URL(fileURLWithPath: "/bin/sh"), arguments, environment)
            if ok { updated.append(target.name) } else { failed.append("\(target.name): \(output)") }
        }
        refresh()
        if !failed.isEmpty {
            report = Report(title: "WineMetalGL \(version) could not be installed everywhere",
                            text: failed.joined(separator: "\n\n"))
        } else if !updated.isEmpty {
            report = Report(title: "WineMetalGL Settings \(version) is installed",
                            text: "WineMetalGL \(version) is now in " + ListFormatter.localizedString(byJoining: updated) + ".")
        } else {
            report = Report(title: "WineMetalGL Settings \(version) is installed", text: "WineMetalGL is not installed in any Wine yet.")
        }
    }

    /// Install: into a Wine found on this Mac, in its default prefix, ~/.wine.
    func install(into found: FoundWine) {
        guard let installer else { return }
        install(installer, wine: found.wine, prefix: home.appendingPathComponent(".wine"), name: "the default prefix (~/.wine)")
    }

    /// Custom Install: any Wine and any prefix, chosen in Finder.
    func installCustom() {
        guard let installer else { return }
        let winePanel = NSOpenPanel()
        winePanel.message = "Choose the Wine to install WineMetalGL into: a Wine app (such as Wine Stable) or the folder with bin/wine."
        winePanel.prompt = "Choose Wine"
        winePanel.canChooseDirectories = true
        winePanel.canChooseFiles = true
        winePanel.allowedContentTypes = [.applicationBundle, .folder]
        winePanel.directoryURL = URL(fileURLWithPath: "/Applications")
        guard winePanel.runModal() == .OK, var wine = winePanel.url else { return }
        if wine.pathExtension == "app" {
            // The app's Wine, wherever in it findWines would look.
            wine = ["Contents/Resources/wine", "Contents/SharedSupport/wine", "Contents/Resources"]
                .map { wine.appendingPathComponent($0) }
                .first { FileManager.default.isExecutableFile(atPath: $0.appendingPathComponent("bin/wine").path) } ?? wine
        }
        guard FileManager.default.isExecutableFile(atPath: wine.appendingPathComponent("bin/wine").path) else {
            report = Report(title: "Not a Wine", text: "\(wine.path) has no bin/wine.")
            return
        }
        let prefixPanel = NSOpenPanel()
        prefixPanel.message = "Choose the Wine prefix whose games should use WineMetalGL (a new folder becomes a new prefix)."
        prefixPanel.prompt = "Choose Prefix"
        prefixPanel.canChooseDirectories = true
        prefixPanel.canChooseFiles = false
        prefixPanel.canCreateDirectories = true
        prefixPanel.showsHiddenFiles = true
        prefixPanel.directoryURL = home
        guard prefixPanel.runModal() == .OK, let prefix = prefixPanel.url else { return }
        install(installer, wine: wine, prefix: prefix, name: prefix.lastPathComponent)
    }

    private func install(_ installer: URL, wine: URL, prefix: URL, name: String) {
        run("Installing WineMetalGL into \(name)…", URL(fileURLWithPath: "/bin/sh"),
            [installer.appendingPathComponent("install-wine-module.sh").path, wine.path, prefix.path], [:]) { [weak self] ok, output in
            guard let self else { return }
            if ok {
                var saved = AppEnvironment.defaults.array(forKey: self.savedKey) as? [[String]] ?? []
                saved.removeAll { $0.first == prefix.path }
                saved.append([prefix.path, wine.path])
                AppEnvironment.defaults.set(saved, forKey: self.savedKey)
            }
            self.finishInstall(ok, output, into: name)
        }
    }

    private func finishInstall(_ ok: Bool, _ output: String, into name: String) {
        refresh()
        report = ok ? Report(title: "WineMetalGL is installed in \(name)", text: "Games started there now use it.")
                    : Report(title: "Installing into \(name) failed", text: output)
    }

    // MARK: Running

    private func run(_ message: String, _ executable: URL, _ arguments: [String], _ environment: [String: String],
                     then done: @escaping @MainActor (Bool, String) -> Void) {
        Task {
            let (ok, output) = await self.perform(message, executable, arguments, environment)
            done(ok, output)
        }
    }

    private func perform(_ message: String, _ executable: URL, _ arguments: [String],
                         _ environment: [String: String]) async -> (Bool, String) {
        busy = message
        defer { busy = nil }
        let (status, output) = await Self.execute(executable, arguments, environment)
        // The end of the output says what went wrong.
        return (status == 0, String(output.suffix(1500)).trimmingCharacters(in: .whitespacesAndNewlines))
    }

    /// Runs a program off the main thread: its exit status and output.
    nonisolated static func execute(_ executable: URL, _ arguments: [String],
                                    _ environment: [String: String] = [:]) async -> (Int32, String) {
        await Task.detached { executeNow(executable, arguments, environment) }.value
    }

    nonisolated static func executeNow(_ executable: URL, _ arguments: [String],
                                       _ environment: [String: String] = [:]) -> (Int32, String) {
        let process = Process()
        process.executableURL = executable
        process.arguments = arguments
        process.environment = AppEnvironment.processEnvironment.merging(environment) { $1 }
        let pipe = Pipe()
        process.standardOutput = pipe
        process.standardError = pipe
        do {
            try process.run()
            let data = pipe.fileHandleForReading.readDataToEndOfFile()
            process.waitUntilExit()
            return (process.terminationStatus, String(decoding: data, as: UTF8.self))
        } catch {
            return (-1, error.localizedDescription)
        }
    }
}

// MARK: - The card

struct InstallationsCard: View {
    @ObservedObject var installations: Installations

    private var summary: String {
        if let busy = installations.busy { return busy }
        let count = installations.targets.filter(\.installed).count
        return count == 0 ? "Not installed" : count == 1 ? "Installed in 1 Wine" : "Installed in \(count) Wines"
    }

    var body: some View {
        CollapsibleCard("WineMetalGL", systemImage: "shippingbox", summary: summary) {
            Button {
                installations.refresh()
            } label: {
                Image(systemName: "arrow.clockwise")
            }
            .buttonStyle(.borderless)
            .help("Look again")
        } content: {
            if !installations.anyInstalled {
                Text("WineMetalGL is not installed in any Wine yet.")
                    .font(.callout)
                    .foregroundStyle(.secondary)
            }
            if !installations.targets.isEmpty {
                ScrollView {
                    VStack(spacing: 8) {
                        ForEach(installations.targets) { target in row(target) }
                    }
                }
                .frame(height: min(CGFloat(installations.targets.count) * 40, 168))
            }
            HStack(spacing: 8) {
                crossoverButton
                installButton
                Button {
                    installations.installCustom()
                } label: {
                    Label("Custom Install", systemImage: "folder.badge.gearshape")
                }
                .glassButton(prominent: false)
                .fixedSize()
                .disabled(installations.installer == nil || installations.busy != nil)
                .help("Choose any Wine in Finder, then its prefix.")
            }
            Text(installations.installer == nil ? "This build of WineMetalGL Settings cannot install WineMetalGL."
                 : installations.carriesRelease ? "Installs the WineMetalGL that comes with this app."
                 : "Downloads the latest WineMetalGL release from GitHub to install.")
                .font(.footnote)
                .foregroundStyle(.tertiary)
            if let busy = installations.busy {
                HStack(spacing: 8) {
                    ProgressView().controlSize(.small)
                    Text(busy).font(.callout).foregroundStyle(.secondary)
                }
            }
        }
        .alert(item: $installations.report) { report in
            Alert(title: Text(report.title), message: Text(report.text))
        }
    }

    /// Install into CrossOver: the bottles found anywhere on the Mac (by copy
    /// of CrossOver when there are several), or one chosen in Finder, with or
    /// without a CrossOver app.
    private var crossoverButton: some View {
        Menu {
            if installations.crossovers.count > 1 {
                ForEach(installations.crossovers, id: \.path) { copy in
                    Menu((Installations.crossoverApp(copy).path as NSString).abbreviatingWithTildeInPath) {
                        ForEach(installations.bottles) { bottle in
                            Button(bottle.name) { installations.installCrossOver(bottle, using: copy) }
                        }
                        Button("Choose Bottle…") { installations.chooseBottle(using: copy) }
                    }
                }
            } else {
                ForEach(installations.bottles) { bottle in
                    Button(bottle.name) { installations.installCrossOver(bottle) }
                }
                if installations.bottles.isEmpty {
                    Text("No CrossOver bottles found")
                }
            }
            Divider()
            Button("Choose Bottle…") { installations.chooseBottle() }
            Button("Choose CrossOver and Bottle…") { installations.installCrossOverCustom() }
        } label: {
            Label("CrossOver", systemImage: "wineglass")
        }
        .glassButton(prominent: !installations.anyInstalled)
        .fixedSize()
        .disabled(installations.installer == nil || installations.busy != nil)
        .help(installations.crossover == nil ? "Install into CrossOver: no CrossOver app found, so WineMetalGL goes into the bottle itself."
              : "Install into CrossOver: choose a bottle, or a CrossOver app and a bottle.")
    }

    /// Install: into the one Wine found, or a menu of them; with none found, Custom Install's choice.
    @ViewBuilder private var installButton: some View {
        let disabled = installations.installer == nil || installations.busy != nil
        if installations.wines.count > 1 {
            Menu {
                ForEach(installations.wines) { found in
                    Button(found.name) { installations.install(into: found) }
                }
            } label: {
                Label("Install", systemImage: "square.and.arrow.down")
            }
            .glassButton(prominent: !installations.anyInstalled)
            .fixedSize()
            .disabled(disabled)
            .help("Choose a Wine: WineMetalGL goes into its default prefix, ~/.wine.")
        } else {
            Button {
                if let found = installations.wines.first {
                    installations.install(into: found)
                } else {
                    installations.installCustom()
                }
            } label: {
                Label("Install", systemImage: "square.and.arrow.down")
            }
            .glassButton(prominent: !installations.anyInstalled)
            .disabled(disabled)
            .help(installations.wines.first.map { "Into \($0.name)'s default prefix, ~/.wine." } ?? "No Wine found: choose one.")
        }
    }

    private func row(_ target: WineTarget) -> some View {
        HStack(spacing: 10) {
            Image(systemName: target.kind == .crossover ? "wineglass" : target.kind == .notproton ? "gamecontroller" : "folder")
                .foregroundStyle(.secondary)
                .frame(width: 18)
            VStack(alignment: .leading, spacing: 1) {
                Text(target.name).lineLimit(1)
                Text(target.prefix.path.replacingOccurrences(of: NSHomeDirectory(), with: "~"))
                    .font(.caption)
                    .foregroundStyle(.secondary)
                    .lineLimit(1)
                    .truncationMode(.middle)
            }
            Spacer()
            if target.installed {
                Toggle(target.name, isOn: Binding(get: { target.enabled },
                                                  set: { installations.setEnabled(target, $0) }))
                    .toggleStyle(.switch)
                    .labelsHidden()
                    .disabled(installations.busy != nil)
                    .help(target.enabled ? "WineMetalGL is on here." : "Wine's own OpenGL is used here.")
            } else if target.kind == .notproton {
                // A NotProton game has no other way in: its runner and prefix come from NotProton.
                Button("Install") { installations.installNotProton(target) }
                    .glassButton(prominent: false)
                    .disabled(installations.installer == nil || installations.busy != nil)
                    .help("Install WineMetalGL into this game's NotProton runner and prefix.")
            } else {
                Text("Not installed").font(.caption).foregroundStyle(.secondary)
            }
        }
    }
}
