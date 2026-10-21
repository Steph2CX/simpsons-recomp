// Draws the "5K / ULTRA HD" logo that replaces "HD" on the start screen ("EA | HD"): white text on a transparent
// 128x64 image, written as raw 8-bit alpha (row by row) for tools/logo_4k.py.
// Usage: swift tools/logo_4k.swift <out.alpha>
import AppKit
import CoreText

let width = 128, height = 64
let context = CGContext(data: nil, width: width, height: height, bitsPerComponent: 8, bytesPerRow: width,
                        space: CGColorSpaceCreateDeviceGray(), bitmapInfo: CGImageAlphaInfo.none.rawValue)!
context.setFillColor(gray: 0, alpha: 1)
context.fill(CGRect(x: 0, y: 0, width: width, height: height))

// Draws one line of text, scaled to fill the given box (in pixels, origin at the bottom left)
func draw(_ text: String, font name: String, in box: CGRect) {
    let font = CTFontCreateWithName(name as CFString, 100, nil)
    let attributes: [NSAttributedString.Key: Any] = [.font: font, .foregroundColor: CGColor(gray: 1, alpha: 1)]
    let line = CTLineCreateWithAttributedString(NSAttributedString(string: text, attributes: attributes))
    let bounds = CTLineGetBoundsWithOptions(line, .useGlyphPathBounds)
    context.saveGState()
    context.translateBy(x: box.minX, y: box.minY)
    context.scaleBy(x: box.width / bounds.width, y: box.height / bounds.height)
    context.translateBy(x: -bounds.minX, y: -bounds.minY)
    context.textPosition = .zero
    CTLineDraw(line, context)
    context.restoreGState()
}

// Same width as the original "HD" (x 9..120); "4K" tall on top, "ULTRA HD" below
draw("5K", font: "DINAlternate-Bold", in: CGRect(x: 40, y: 30, width: 48, height: 30))
draw("ULTRA HD", font: "DINAlternate-Bold", in: CGRect(x: 9, y: 6, width: 112, height: 18))

let pixels = context.data!.assumingMemoryBound(to: UInt8.self)
// The bitmap memory starts with the top row
var out = [UInt8](repeating: 0, count: width * height)
for y in 0..<height {
    for x in 0..<width {
        out[y * width + x] = pixels[y * width + x]
    }
}
FileManager.default.createFile(atPath: CommandLine.arguments[1], contents: Data(out))
