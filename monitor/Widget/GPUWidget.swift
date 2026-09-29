// Desktop / Notification Center widget. Reads the registry directly when the sandbox allows
// it, else the app's snapshot; the sparkline always comes from the snapshot.
import SwiftUI
import Charts
import WidgetKit

struct GPUEntry: TimelineEntry {
    var date: Date
    var name: String
    var state: GPUState
    var detail: String
    var sample: GPUSample?
    var history: [GPUSample]
}

struct Provider: TimelineProvider {
    func placeholder(in context: Context) -> GPUEntry {
        GPUEntry(date: Date(), name: "NVIDIA GeForce GTX 1650", state: .running, detail: "GSP-RM running",
                 sample: GPUSample(time: Date(), utilization: 42, temperature: 58, vramUsed: 1 << 30,
                                   vramTotal: 3 << 30, bar1Used: 0, bar1Total: 0, connections: 1, contexts: 1),
                 history: [])
    }

    func getSnapshot(in context: Context, completion: @escaping (GPUEntry) -> Void) {
        completion(context.isPreview ? placeholder(in: context) : current())
    }

    func getTimeline(in context: Context, completion: @escaping (Timeline<GPUEntry>) -> Void) {
        // The app asks for reloads while it runs; this is the fallback when it doesn't.
        completion(Timeline(entries: [current()], policy: .after(Date().addingTimeInterval(60))))
    }

    private func current() -> GPUEntry {
        let now = Date()
        let snap = SharedStore.load()
        let live = GPUReader.read(now: now)
        if live.state != .noDriver || snap == nil {
            return GPUEntry(date: now, name: live.name, state: live.state, detail: live.detail,
                            sample: live.sample, history: snap?.history ?? [])
        }
        // No registry access from here: use what the app saw, if it is recent.
        let s = snap!
        let fresh = now.timeIntervalSince(s.updated) < 120
        return GPUEntry(date: now, name: s.name, state: fresh ? s.state : .stale,
                        detail: fresh ? s.detail : "Open GPU Monitor to refresh",
                        sample: fresh ? s.history.last : nil, history: s.history)
    }
}

struct GPUWidgetView: View {
    @Environment(\.widgetFamily) private var family
    var entry: GPUEntry

    var body: some View {
        let s = entry.sample
        switch family {
        case .systemSmall:
            VStack(alignment: .leading, spacing: 8) {
                header
                HStack(spacing: 10) {
                    RingGauge(fraction: s?.utilization.map { $0 / 100 }, label: "Usage",
                              value: Fmt.pct(s?.utilization), color: Level.load(s?.utilization.map { $0 / 100 }), lineWidth: 6)
                    RingGauge(fraction: s?.temperature.map { ($0 - 30) / 70 }, label: "Temp",
                              value: Fmt.temp(s?.temperature), color: Level.temperature(s?.temperature), lineWidth: 6)
                }
                UsageBar(label: "VRAM", used: s?.vramUsed ?? 0, total: s?.vramTotal ?? 0)
            }
        default:
            VStack(alignment: .leading, spacing: 8) {
                header
                HStack(spacing: 12) {
                    RingGauge(fraction: s?.utilization.map { $0 / 100 }, label: "Usage",
                              value: Fmt.pct(s?.utilization), color: Level.load(s?.utilization.map { $0 / 100 }), lineWidth: 6)
                        .frame(width: 72)
                    RingGauge(fraction: s?.temperature.map { ($0 - 30) / 70 }, label: "Temp",
                              value: Fmt.temp(s?.temperature), color: Level.temperature(s?.temperature), lineWidth: 6)
                        .frame(width: 72)
                    VStack(alignment: .leading, spacing: 6) {
                        sparkline
                        UsageBar(label: "VRAM", used: s?.vramUsed ?? 0, total: s?.vramTotal ?? 0)
                    }
                    .frame(maxWidth: .infinity)
                }
            }
        }
    }

    private var header: some View {
        HStack(spacing: 5) {
            Circle().fill(Level.state(entry.state)).frame(width: 6, height: 6)
            Text(entry.name.replacingOccurrences(of: "NVIDIA GeForce ", with: ""))
                .font(.caption.weight(.semibold)).lineLimit(1)
            Spacer(minLength: 0)
            Text(entry.date, style: .time).font(.caption2).foregroundStyle(.secondary)
        }
    }

    @ViewBuilder private var sparkline: some View {
        let pts = entry.history.filter { $0.utilization != nil }
        if pts.count >= 2 {
            Chart(pts, id: \.time) { s in
                AreaMark(x: .value("Time", s.time), y: .value("Usage", s.utilization!))
                    .foregroundStyle(Color.accentColor.opacity(0.2))
                    .interpolationMethod(.monotone)
                LineMark(x: .value("Time", s.time), y: .value("Usage", s.utilization!))
                    .foregroundStyle(Color.accentColor)
                    .interpolationMethod(.monotone)
            }
            .chartYScale(domain: 0...100)
            .chartXAxis(.hidden)
            .chartYAxis(.hidden)
        } else {
            Text(entry.state == .running || entry.state == .demo ? "Collecting history…" : entry.detail)
                .font(.caption2).foregroundStyle(.secondary)
                .frame(maxWidth: .infinity, maxHeight: .infinity)
        }
    }
}

struct GPUWidget: Widget {
    var body: some WidgetConfiguration {
        StaticConfiguration(kind: "GPUWidget", provider: Provider()) { entry in
            GPUWidgetView(entry: entry)
                .containerBackground(.background, for: .widget)
        }
        .configurationDisplayName("GPU Monitor")
        .description("NVIDIA GPU usage, temperature and VRAM from NVBringup.")
        .supportedFamilies([.systemSmall, .systemMedium])
    }
}

@main
struct GPUWidgetBundle: WidgetBundle {
    var body: some Widget { GPUWidget() }
}
