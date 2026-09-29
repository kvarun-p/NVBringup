// GPU metrics from NVBringup's registry properties (src/NVGpu.cpp updateStats), shared by
// the menu bar app and the widget. Reads only named properties: the kext also publishes
// large ones (NVLog, the ROM) that a 1 s poll must not copy.
import Foundation
import IOKit

struct GPUSample: Codable, Hashable {
    var time: Date
    var utilization: Double?        // % of 100 ms samples with work in flight, over 1 s
    var temperature: Double?        // °C, PTHERM sensor
    var vramUsed: UInt64
    var vramTotal: UInt64
    var bar1Used: UInt64
    var bar1Total: UInt64
    var connections: Int
    var contexts: Int

    var vramFraction: Double { vramTotal > 0 ? Double(vramUsed) / Double(vramTotal) : 0 }
    var bar1Fraction: Double { bar1Total > 0 ? Double(bar1Used) / Double(bar1Total) : 0 }
}

enum GPUState: String, Codable {
    case running        // stats flowing
    case stale          // NVStats stopped updating
    case noStats        // GSP-RM running, but the kext predates NVStats
    case gspDown        // driver loaded, GSP-RM not running
    case poweredOff     // GPU cut off while idle (NVPower, src/NVPower.cpp)
    case switching      // powering on or off
    case noDriver       // NVBringup not in the registry
    case demo
}

struct GPUReading {
    var name: String
    var state: GPUState
    var detail: String
    var sample: GPUSample?
    var sequence: Int?
    var power: GPUPowerInfo? = nil
}

// NVPower: runtime power state and mode (the kext cuts the GPU off after IdleSeconds unused).
struct GPUPowerInfo: Equatable {
    var state: String           // on, off, switching, failed
    var mode: String            // auto, on, off
    var idleSeconds: Int
    var lastPowerOnMs: Int
    var capable: Bool = true    // ACPI can cut this GPU's power (laptops); desktop cards: false
}

enum GPUPower {
    static let modes = ["auto", "on", "off"]

    // Sets the mode through the kext's control client (IOServiceOpen type 2, src/nv_uapi.h),
    // which doesn't open the GPU. Blocks while the GPU powers on (a few seconds): call off the
    // main thread. Returns nil or an error text.
    static func set(_ mode: String) -> String? {
        let value: UInt64 = mode == "off" ? 0 : mode == "on" ? 1 : 2
        let svc = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching("NVBringup"))
        guard svc != 0 else { return "NVBringup is not loaded" }
        defer { IOObjectRelease(svc) }
        var conn: io_connect_t = 0
        var kr = IOServiceOpen(svc, mach_task_self_, 2, &conn)
        guard kr == KERN_SUCCESS else { return String(format: "control client: 0x%x", kr) }
        defer { IOServiceClose(conn) }
        var input = value
        var out = [UInt64](repeating: 0, count: 7)
        var n: UInt32 = 7
        kr = IOConnectCallScalarMethod(conn, 1, &input, 1, &out, &n)
        if kr == kIOReturnBusy { return "An app has the GPU open" }
        return kr == KERN_SUCCESS ? nil : String(format: "power call: 0x%x", kr)
    }
}

enum GPUReader {
    static let demo = CommandLine.arguments.contains("--demo") || ProcessInfo.processInfo.environment["GPUMON_DEMO"] == "1"

    static func read(now: Date = Date()) -> GPUReading {
        if demo { return demoReading(now) }
        let svc = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching("NVBringup"))
        guard svc != 0 else {
            return GPUReading(name: "NVIDIA GPU", state: .noDriver, detail: "NVBringup is not loaded", sample: nil, sequence: nil)
        }
        defer { IOObjectRelease(svc) }
        func prop(_ key: String) -> Any? {
            IORegistryEntryCreateCFProperty(svc, key as CFString, kCFAllocatorDefault, 0)?.takeRetainedValue()
        }
        let name = (prop("NVGpuName") as? String) ?? "NVIDIA GPU"
        var power: GPUPowerInfo?
        if let p = prop("NVPower") as? [String: Any] {
            power = GPUPowerInfo(state: p["State"] as? String ?? "on", mode: p["Mode"] as? String ?? "auto",
                                 idleSeconds: (p["IdleSeconds"] as? NSNumber)?.intValue ?? 0,
                                 lastPowerOnMs: (p["LastPowerOnMs"] as? NSNumber)?.intValue ?? 0,
                                 capable: (p["Capable"] as? Bool) ?? true)
        }
        switch power?.state {
        case "off":
            let detail = power?.mode == "off" ? "Powered off (switched off)" : "Powered off · wakes when an app uses it"
            return GPUReading(name: name, state: .poweredOff, detail: detail, sample: nil, sequence: nil, power: power)
        case "switching":
            return GPUReading(name: name, state: .switching, detail: "Switching power…", sample: nil, sequence: nil, power: power)
        default:
            break
        }
        let gsp = (prop("NVGspResult") as? String) ?? "not booted"
        guard gsp == "running" else {
            return GPUReading(name: name, state: .gspDown, detail: "GSP-RM \(gsp)", sample: nil, sequence: nil, power: power)
        }
        guard let s = prop("NVStats") as? [String: Any] else {
            return GPUReading(name: name, state: .noStats, detail: "No NVStats: reboot into the updated kext",
                              sample: nil, sequence: nil)
        }
        func num(_ k: String) -> NSNumber? { s[k] as? NSNumber }
        let sample = GPUSample(
            time: now,
            utilization: num("Utilization")?.doubleValue,
            temperature: num("Temperature").map { $0.doubleValue / 100 },
            vramUsed: num("VramUsed")?.uint64Value ?? 0,
            vramTotal: num("VramTotal")?.uint64Value ?? 0,
            bar1Used: num("Bar1Used")?.uint64Value ?? 0,
            bar1Total: num("Bar1Total")?.uint64Value ?? 0,
            connections: num("Connections")?.intValue ?? 0,
            contexts: num("Contexts")?.intValue ?? 0)
        return GPUReading(name: name, state: .running, detail: "GSP-RM running", sample: sample,
                          sequence: num("Sequence")?.intValue, power: power)
    }

    // Synthetic data for trying the UI without the GPU (--demo or GPUMON_DEMO=1).
    private static func demoReading(_ now: Date) -> GPUReading {
        let t: Double = now.timeIntervalSinceReferenceDate
        let wave: Double = 40 * sin(t / 20)
        let noise: Double = Double.random(in: -8...8)
        let load: Double = max(0, min(100, 55 + wave + noise))
        let temp: Double = 48 + load * 0.3
        let vram: Double = 1.4e9 + 6e8 * sin(t / 45)
        let mib: UInt64 = 1 << 20
        let sample = GPUSample(time: now, utilization: (load / 10).rounded() * 10, temperature: temp,
                               vramUsed: UInt64(vram), vramTotal: 3_900_000_000,
                               bar1Used: 40 * mib, bar1Total: 240 * mib, connections: 2, contexts: 3)
        return GPUReading(name: "NVIDIA GeForce GTX 1650", state: .demo, detail: "Demo data",
                          sample: sample, sequence: Int(t))
    }
}

// What the app hands the widget: the latest state and a short history. The widget is
// sandboxed and may not reach the registry, so this file is its fallback and its sparkline.
struct SharedSnapshot: Codable {
    var name: String
    var state: GPUState
    var detail: String
    var updated: Date
    var history: [GPUSample]
}

enum SharedStore {
    static let relativePath = "Library/Application Support/GPUMonitor"

    // The real home: inside the widget's sandbox NSHomeDirectory() is its container.
    static var directory: URL {
        let home = getpwuid(getuid()).flatMap { String(cString: $0.pointee.pw_dir) } ?? NSHomeDirectory()
        return URL(fileURLWithPath: home).appendingPathComponent(relativePath, isDirectory: true)
    }
    static var file: URL { directory.appendingPathComponent("snapshot.json") }

    static func save(_ s: SharedSnapshot) {
        do {
            try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
            try JSONEncoder().encode(s).write(to: file, options: .atomic)
        } catch {
            NSLog("GPUMonitor: saving snapshot failed: \(error)")
        }
    }

    static func load() -> SharedSnapshot? {
        guard let d = try? Data(contentsOf: file) else { return nil }
        return try? JSONDecoder().decode(SharedSnapshot.self, from: d)
    }
}

enum Fmt {
    static func bytes(_ b: UInt64) -> String {
        let g = Double(b) / 1_073_741_824
        return g >= 1 ? String(format: "%.2f GiB", g) : String(format: "%.0f MiB", Double(b) / 1_048_576)
    }
    // "1.78 / 3.63 GiB": one unit for both, from the total.
    static func pair(_ used: UInt64, _ total: UInt64) -> String {
        let gib = Double(total) >= 1_073_741_824
        let d = gib ? 1_073_741_824.0 : 1_048_576.0
        return String(format: gib ? "%.2f / %.2f GiB" : "%.0f / %.0f MiB", Double(used) / d, Double(total) / d)
    }
    // Menu bar form: "482M", "1.2G".
    static func short(_ b: UInt64) -> String {
        let m = Double(b) / 1_048_576
        return m >= 1024 ? String(format: "%.1fG", m / 1024) : String(format: "%.0fM", m)
    }
    static func pct(_ v: Double?) -> String { v.map { String(format: "%.0f%%", $0) } ?? "–" }
    static func temp(_ v: Double?) -> String { v.map { String(format: "%.0f°C", $0) } ?? "–" }
}
