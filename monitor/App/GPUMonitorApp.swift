// Menu bar app: polls NVBringup's NVStats every second, keeps 5 minutes of history, and
// hands a snapshot to the widget.
import SwiftUI
import Charts
import WidgetKit
import ServiceManagement

@main
struct GPUMonitorApp: App {
    @StateObject private var model = MonitorModel()

    init() {
        // One copy only: a duplicate login item must not open a second menu bar item.
        let me = ProcessInfo.processInfo.processIdentifier
        if NSRunningApplication.runningApplications(withBundleIdentifier: Bundle.main.bundleIdentifier ?? "")
            .contains(where: { $0.processIdentifier != me }) {
            exit(0)
        }
        LoginItem.sync()
    }

    var body: some Scene {
        MenuBarExtra {
            PopoverView(model: model)
        } label: {
            MenuBarLabel(reading: model.reading)
        }
        .menuBarExtraStyle(.window)
    }
}

@MainActor
final class MonitorModel: ObservableObject {
    @Published private(set) var reading: GPUReading
    @Published private(set) var history: [GPUSample] = []
    @Published private(set) var powerBusy = false
    @Published private(set) var powerError: String?

    private static let keep: TimeInterval = 300
    private var timer: Timer?
    private var lastSequence: Int?
    private var lastSequenceChange = Date()
    private var lastSave = Date.distantPast
    private var lastWidgetReload = Date.distantPast

    init() {
        reading = GPUReader.read()
        timer = Timer.scheduledTimer(withTimeInterval: 1, repeats: true) { [weak self] _ in
            guard let self else { return }
            Task { @MainActor in self.tick() }
        }
        timer?.tolerance = 0.1
        tick()
    }

    private func tick() {
        let now = Date()
        var r = GPUReader.read(now: now)
        // The kext publishes once a second while GSP-RM runs; a frozen sequence means the
        // poller stopped.
        if r.state == .running {
            if r.sequence != lastSequence {
                lastSequence = r.sequence
                lastSequenceChange = now
            } else if now.timeIntervalSince(lastSequenceChange) > 5 {
                r.state = .stale
                r.detail = "NVStats stopped updating"
            }
        }
        reading = r
        if let s = r.sample, r.state == .running || r.state == .demo {
            history.append(s)
        }
        history.removeAll { now.timeIntervalSince($0.time) > Self.keep }

        if now.timeIntervalSince(lastSave) >= 5 {
            lastSave = now
            SharedStore.save(SharedSnapshot(name: r.name, state: r.state, detail: r.detail, updated: now,
                                            history: Self.downsample(history, every: 5)))
            // macOS rate-limits widget reloads; asking every 15 s is as fresh as it gets.
            if now.timeIntervalSince(lastWidgetReload) >= 15 {
                lastWidgetReload = now
                WidgetCenter.shared.reloadAllTimelines()
            }
        }
    }

    func setPower(_ mode: String) {
        guard !powerBusy else { return }
        powerBusy = true
        powerError = nil
        Task.detached {
            let err = GPUPower.set(mode)        // blocks while the GPU powers on
            await MainActor.run {
                self.powerBusy = false
                self.powerError = err
                self.tick()
            }
        }
    }

    private static func downsample(_ h: [GPUSample], every n: Int) -> [GPUSample] {
        guard h.count > n else { return h }
        return stride(from: h.count - 1, through: 0, by: -n).map { h[$0] }.reversed()
    }
}

struct MenuBarLabel: View {
    var reading: GPUReading

    var body: some View {
        if let s = reading.sample, reading.state == .running || reading.state == .demo {
            // Titled columns like the system's and Stats' items: GPU usage, temperature (when the
            // sensor reads), VRAM used.
            var cols = [("GPU", Fmt.pct(s.utilization))]
            if let t = s.temperature { cols.append(("TEMP", String(format: "%.0f°", t))) }
            cols.append(("VRAM", Fmt.short(s.vramUsed)))
            return AnyView(Image(nsImage: MenuBarImage.render(cols)))
        } else if reading.state == .poweredOff {
            return AnyView(Image(systemName: "powersleep"))     // GPU cut off while idle
        } else {
            return AnyView(Image(systemName: "cpu"))
        }
    }
}

// A menu bar label only shows one line of text or an image, so the two-row columns are drawn
// into a template image (the menu bar tints it for light and dark).
@MainActor
enum MenuBarImage {
    static func render(_ cols: [(String, String)]) -> NSImage {
        let view = HStack(alignment: .bottom, spacing: 6) {
            ForEach(Array(cols.enumerated()), id: \.offset) { _, c in
                VStack(alignment: .leading, spacing: -1) {
                    Text(c.0).font(.system(size: 7, weight: .semibold))
                    Text(c.1).font(.system(size: 11, weight: .medium)).monospacedDigit()
                }
                .frame(minWidth: 26, alignment: .leading)       // steady width as values change
            }
        }
        .foregroundStyle(.black)
        .padding(.horizontal, 1)
        .frame(height: 22)
        let r = ImageRenderer(content: view)
        r.scale = NSScreen.main?.backingScaleFactor ?? 2
        let img = r.nsImage ?? NSImage()
        img.isTemplate = true
        return img
    }
}

struct PopoverView: View {
    @ObservedObject var model: MonitorModel

    var body: some View {
        let r = model.reading
        let s = r.sample
        VStack(alignment: .leading, spacing: 14) {
            HStack(alignment: .firstTextBaseline) {
                VStack(alignment: .leading, spacing: 2) {
                    Text(r.name).font(.headline)
                    HStack(spacing: 5) {
                        Circle().fill(Level.state(r.state)).frame(width: 7, height: 7)
                        Text(r.detail).font(.caption).foregroundStyle(.secondary)
                    }
                }
                Spacer()
            }

            HStack(spacing: 18) {
                RingGauge(fraction: s?.utilization.map { $0 / 100 }, label: "Usage",
                          value: Fmt.pct(s?.utilization), color: Level.load(s?.utilization.map { $0 / 100 }))
                RingGauge(fraction: s?.temperature.map { ($0 - 30) / 70 }, label: "Temperature",
                          value: Fmt.temp(s?.temperature), color: Level.temperature(s?.temperature))
                RingGauge(fraction: s?.vramFraction, label: "VRAM",
                          value: s.map { Fmt.pct($0.vramFraction * 100) } ?? "–", color: Level.load(s?.vramFraction))
            }
            .frame(height: 96)

            VStack(spacing: 8) {
                UsageBar(label: "VRAM (driver heap)", used: s?.vramUsed ?? 0, total: s?.vramTotal ?? 0)
                UsageBar(label: "BAR1 (CPU-visible)", used: s?.bar1Used ?? 0, total: s?.bar1Total ?? 0)
            }

            HistoryChart(history: model.history)

            PowerRow(model: model)

            Divider()
            HStack {
                if let s {
                    Text("\(s.connections) client\(s.connections == 1 ? "" : "s") · \(s.contexts) context\(s.contexts == 1 ? "" : "s")")
                        .font(.caption).foregroundStyle(.secondary)
                }
                Spacer()
                Toggle("Launch at login", isOn: Binding(
                    get: { LoginItem.wanted },
                    set: { on in
                        LoginItem.set(on)
                        model.objectWillChange.send()
                    }))
                    .toggleStyle(.checkbox)
                    .font(.caption)
                    .help(LoginItem.needsApproval ? "Allow GPU Monitor in System Settings › General › Login Items" : "")
                Button("Quit") { NSApp.terminate(nil) }
                    .keyboardShortcut("q")
            }
        }
        .padding(16)
        .frame(width: 340)
    }
}

// GPU power mode: Auto cuts the GPU off after the idle time without clients and powers it on
// when an app opens it (a few seconds); On keeps it powered; Off cuts it now and keeps it off.
struct PowerRow: View {
    @ObservedObject var model: MonitorModel

    var body: some View {
        let p = model.reading.power
        VStack(alignment: .leading, spacing: 4) {
            HStack {
                Text("GPU power").font(.caption).foregroundStyle(.secondary)
                Spacer()
                if model.powerBusy { ProgressView().controlSize(.small) }
                Picker("", selection: Binding(
                    get: { p?.mode ?? "auto" },
                    set: { model.setPower($0) })) {
                    Text("Auto").tag("auto")
                    Text("On").tag("on")
                    Text("Off").tag("off")
                }
                .pickerStyle(.segmented)
                .labelsHidden()
                .frame(width: 150)
                .disabled(p == nil || model.powerBusy || p?.capable == false)
            }
            if let err = model.powerError {
                Text(err).font(.caption2).foregroundStyle(.red)
            } else if let p, !p.capable {
                Text("Always on: this GPU has no ACPI power control").font(.caption2).foregroundStyle(.tertiary)
            } else if let p {
                Text(p.mode == "auto" ? "Off after \(p.idleSeconds) s unused; last power-on \(p.lastPowerOnMs) ms"
                     : p.mode == "on" ? "Kept on" : "Kept off: apps can't use the GPU")
                    .font(.caption2).foregroundStyle(.tertiary)
            } else {
                Text("Needs the kext with runtime power").font(.caption2).foregroundStyle(.tertiary)
            }
        }
    }
}

// Launch at login through SMAppService. macOS identifies this ad-hoc signed app by its code hash,
// which changes with every build, so a rebuilt app reads as unregistered even though you turned
// the option on. The choice is therefore kept in UserDefaults, shown on the checkbox, and applied
// again at every launch (re-registering the current build).
enum LoginItem {
    private static let key = "launchAtLogin"

    static var wanted: Bool {
        if let v = UserDefaults.standard.object(forKey: key) as? Bool { return v }
        return SMAppService.mainApp.status == .enabled      // before this was remembered
    }

    static var needsApproval: Bool { SMAppService.mainApp.status == .requiresApproval }

    static func set(_ on: Bool) {
        UserDefaults.standard.set(on, forKey: key)
        apply(on)
        if on && needsApproval { SMAppService.openSystemSettingsLoginItems() }
    }

    // At launch: make the registration match the remembered choice.
    static func sync() {
        guard UserDefaults.standard.object(forKey: key) != nil || SMAppService.mainApp.status == .enabled else { return }
        apply(wanted)
    }

    private static func apply(_ on: Bool) {
        let enabled = SMAppService.mainApp.status == .enabled
        do {
            if on && !enabled { try SMAppService.mainApp.register() }
            if !on && enabled { try SMAppService.mainApp.unregister() }
        } catch {
            NSLog("GPUMonitor: login item: \(error)")
        }
    }
}

struct HistoryChart: View {
    var history: [GPUSample]

    var body: some View {
        VStack(alignment: .leading, spacing: 6) {
            HStack {
                Text("Last 5 minutes").font(.caption).foregroundStyle(.secondary)
                Spacer()
                legend("Usage %", .accentColor)
                legend("°C", .orange)
            }
            if history.count < 2 {
                Text("Collecting…").font(.caption).foregroundStyle(.tertiary)
                    .frame(maxWidth: .infinity, minHeight: 90)
            } else {
                Chart {
                    ForEach(history, id: \.time) { s in
                        if let u = s.utilization {
                            AreaMark(x: .value("Time", s.time), y: .value("Usage", u), series: .value("Metric", "Usage"))
                                .foregroundStyle(Color.accentColor.opacity(0.18))
                                .interpolationMethod(.monotone)
                            LineMark(x: .value("Time", s.time), y: .value("Value", u), series: .value("Metric", "Usage"))
                                .foregroundStyle(Color.accentColor)
                                .interpolationMethod(.monotone)
                        }
                        if let t = s.temperature {
                            LineMark(x: .value("Time", s.time), y: .value("Value", t), series: .value("Metric", "Temp"))
                                .foregroundStyle(Color.orange)
                                .lineStyle(StrokeStyle(lineWidth: 1.5))
                                .interpolationMethod(.monotone)
                        }
                    }
                }
                .chartYScale(domain: 0...100)
                .chartYAxis { AxisMarks(values: [0, 50, 100]) }
                .chartXAxis(.hidden)
                .frame(height: 90)
            }
        }
    }

    private func legend(_ text: String, _ color: Color) -> some View {
        HStack(spacing: 3) {
            Circle().fill(color).frame(width: 6, height: 6)
            Text(text).font(.caption2).foregroundStyle(.secondary)
        }
    }
}
