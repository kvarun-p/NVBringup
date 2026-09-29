// Draws GPU Monitor's app icon: a GPU chip whose die carries a usage ring, on a dark macOS-style
// rounded square. Writes a 1024 px PNG; build_icon.sh turns it into AppIcon.icns.
//   swiftc -parse-as-library make_icon.swift -o make_icon && ./make_icon icon_1024.png
import AppKit
import SwiftUI

struct Icon: View {
    var body: some View {
        ZStack {
            // macOS icon grid: 824 pt rounded square inside the 1024 canvas.
            RoundedRectangle(cornerRadius: 185, style: .continuous)
                .fill(LinearGradient(colors: [Color(red: 0.16, green: 0.19, blue: 0.26),
                                              Color(red: 0.06, green: 0.07, blue: 0.10)],
                                     startPoint: .top, endPoint: .bottom))
                .overlay(RoundedRectangle(cornerRadius: 185, style: .continuous)
                    .strokeBorder(Color.white.opacity(0.10), lineWidth: 4))
                .frame(width: 824, height: 824)
                .shadow(color: .black.opacity(0.35), radius: 18, y: 10)

            Pins().frame(width: 560, height: 560)

            // The package.
            RoundedRectangle(cornerRadius: 56, style: .continuous)
                .fill(LinearGradient(colors: [Color(red: 0.24, green: 0.27, blue: 0.33),
                                              Color(red: 0.15, green: 0.17, blue: 0.22)],
                                     startPoint: .topLeading, endPoint: .bottomTrailing))
                .overlay(RoundedRectangle(cornerRadius: 56, style: .continuous)
                    .strokeBorder(Color.white.opacity(0.16), lineWidth: 5))
                .frame(width: 470, height: 470)

            // The usage ring on the die: a track and a 72 % arc, green to yellow.
            Circle()
                .stroke(Color.white.opacity(0.10), lineWidth: 44)
                .frame(width: 300, height: 300)
            Circle()
                .trim(from: 0, to: 0.72)
                .stroke(AngularGradient(colors: [Color(red: 0.20, green: 0.85, blue: 0.55),
                                                 Color(red: 0.55, green: 0.90, blue: 0.30),
                                                 Color(red: 1.00, green: 0.80, blue: 0.25)],
                                        center: .center, startAngle: .degrees(0), endAngle: .degrees(260)),
                        style: StrokeStyle(lineWidth: 44, lineCap: .round))
                .rotationEffect(.degrees(-90))
                .frame(width: 300, height: 300)
                .shadow(color: Color(red: 0.3, green: 0.9, blue: 0.5).opacity(0.45), radius: 16)

            // Three bars in the middle: a small activity graph.
            HStack(alignment: .bottom, spacing: 18) {
                ForEach([60.0, 104.0, 80.0], id: \.self) { h in
                    RoundedRectangle(cornerRadius: 9).fill(Color.white.opacity(0.88)).frame(width: 30, height: h)
                }
            }
            .offset(y: 12)
        }
        .frame(width: 1024, height: 1024)
    }
}

// Six pins on each side of the package.
struct Pins: View {
    var body: some View {
        GeometryReader { g in
            let s = g.size.width, n = 6, pitch = s * 0.62 / CGFloat(n - 1), start = s * 0.19
            ZStack {
                ForEach(0..<n, id: \.self) { i in
                    let p = start + CGFloat(i) * pitch
                    pin.position(x: p, y: 22)
                    pin.position(x: p, y: s - 22)
                    pin.rotationEffect(.degrees(90)).position(x: 22, y: p)
                    pin.rotationEffect(.degrees(90)).position(x: s - 22, y: p)
                }
            }
        }
    }
    var pin: some View {
        RoundedRectangle(cornerRadius: 7)
            .fill(LinearGradient(colors: [Color(white: 0.85), Color(white: 0.55)], startPoint: .top, endPoint: .bottom))
            .frame(width: 30, height: 60)
    }
}

@main
struct Main {
    @MainActor static func main() {
        let r = ImageRenderer(content: Icon())
        r.scale = 1
        guard let cg = r.cgImage,
              let png = NSBitmapImageRep(cgImage: cg).representation(using: .png, properties: [:]) else {
            fputs("render failed\n", stderr)
            exit(1)
        }
        try! png.write(to: URL(fileURLWithPath: CommandLine.arguments.count > 1 ? CommandLine.arguments[1] : "icon_1024.png"))
    }
}
