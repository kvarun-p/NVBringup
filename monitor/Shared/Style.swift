// Colors and small views shared by the app and the widget.
import SwiftUI

enum Level {
    // Turing GPUs slow down from about 83–87 °C (GTX 1650 mobile: ~87 °C); orange from 75 °C.
    static func temperature(_ t: Double?) -> Color {
        guard let t else { return .secondary }
        return t < 75 ? .green : t < 87 ? .orange : .red
    }
    static func load(_ f: Double?) -> Color {
        guard let f else { return .secondary }
        return f < 0.75 ? .accentColor : f < 0.9 ? .orange : .red
    }
    static func state(_ s: GPUState) -> Color {
        switch s {
        case .running: return .green
        case .demo: return .blue
        case .stale, .noStats: return .orange
        case .gspDown, .noDriver: return .red
        case .poweredOff: return .gray
        case .switching: return .yellow
        }
    }
}

// A circular gauge with the value in the middle.
struct RingGauge: View {
    var fraction: Double?           // 0...1, nil = no data
    var label: String
    var value: String
    var color: Color
    var lineWidth: CGFloat = 7

    var body: some View {
        VStack(spacing: 4) {
            // Square, and inset by half the line width: a stroke is centered on the path, so an
            // uninset circle spills past its frame and gets clipped at the popover's edge.
            ZStack {
                Circle().inset(by: lineWidth / 2).stroke(color.opacity(0.18), lineWidth: lineWidth)
                Circle().inset(by: lineWidth / 2)
                    .trim(from: 0, to: max(0, min(1, fraction ?? 0)))
                    .stroke(color, style: StrokeStyle(lineWidth: lineWidth, lineCap: .round))
                    .rotationEffect(.degrees(-90))
                Text(value)
                    .font(.system(.title3, design: .rounded).weight(.semibold))
                    .monospacedDigit()
                    .minimumScaleFactor(0.6)
                    .lineLimit(1)
                    .padding(.horizontal, lineWidth + 2)
            }
            .aspectRatio(1, contentMode: .fit)
            Text(label).font(.caption).foregroundStyle(.secondary)
        }
        .frame(maxWidth: .infinity)                 // equal shares of the row, centered
    }
}

// A labeled horizontal bar: used / total.
struct UsageBar: View {
    var label: String
    var used: UInt64
    var total: UInt64

    var fraction: Double { total > 0 ? Double(used) / Double(total) : 0 }

    var body: some View {
        VStack(alignment: .leading, spacing: 3) {
            HStack {
                Text(label).font(.caption).foregroundStyle(.secondary)
                Spacer()
                Text(total > 0 ? Fmt.pair(used, total) : "–")
                    .font(.caption).monospacedDigit()
                    .lineLimit(1)
            }
            GeometryReader { geo in
                ZStack(alignment: .leading) {
                    Capsule().fill(Color.secondary.opacity(0.18))
                    Capsule().fill(Level.load(fraction))
                        .frame(width: max(total > 0 ? 4 : 0, geo.size.width * fraction))
                }
            }
            .frame(height: 6)
        }
    }
}
