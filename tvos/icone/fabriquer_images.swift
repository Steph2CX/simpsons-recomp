// Fabrique les images de l'icone Apple TV a partir d'une capture 4K de l'ecran titre et du donut de l'icone Mac :
//   swift fabriquer_images.swift <titre 3840x2160.png> <donut.png> <dossier Assets.xcassets>
// Icone en deux calques (ciel nuageux derriere, donut devant : effet de relief au survol), images Top Shelf (logo).
import AppKit

let arguments = CommandLine.arguments
guard arguments.count == 4,
      let title = NSImage(contentsOfFile: arguments[1])?.cgImage(forProposedRect: nil, context: nil, hints: nil),
      let donut = NSImage(contentsOfFile: arguments[2])?.cgImage(forProposedRect: nil, context: nil, hints: nil) else {
    print("usage : swift fabriquer_images.swift <titre 3840x2160.png> <donut.png> <Assets.xcassets>")
    exit(1)
}
let assets = URL(fileURLWithPath: arguments[3])
let brand = assets.appendingPathComponent("App Icon & Top Shelf Image.brandassets")

func write(_ json: String, _ folder: URL) {
    try! FileManager.default.createDirectory(at: folder, withIntermediateDirectories: true)
    try! json.write(to: folder.appendingPathComponent("Contents.json"), atomically: true, encoding: .utf8)
}

// Draws into a width x height image (no alpha when opaque) and saves it as PNG
func render(_ width: Int, _ height: Int, opaque: Bool, to url: URL, _ draw: (CGContext) -> Void) {
    let context = CGContext(data: nil, width: width, height: height, bitsPerComponent: 8, bytesPerRow: 0,
                            space: CGColorSpace(name: CGColorSpace.sRGB)!,
                            bitmapInfo: opaque ? CGImageAlphaInfo.noneSkipLast.rawValue : CGImageAlphaInfo.premultipliedLast.rawValue)!
    context.interpolationQuality = .high
    draw(context)
    let rep = NSBitmapImageRep(cgImage: context.makeImage()!)
    try! rep.representation(using: .png, properties: [:])!.write(to: url)
}

// Part of the title screen (top-left origin, in 4K pixels) scaled to fill the image
func titleCrop(_ x: Int, _ y: Int, _ w: Int, _ h: Int) -> (CGContext) -> Void {
    let crop = title.cropping(to: CGRect(x: x, y: y, width: w, height: h))!
    return { context in context.draw(crop, in: CGRect(x: 0, y: 0, width: context.width, height: context.height)) }
}

let sky = titleCrop(2640, 0, 1200, 720) // clouds at the top right, away from the logo
let front: (CGContext) -> Void = { context in
    let side = Double(context.height) * 0.86
    context.setShadow(offset: CGSize(width: 0, height: -side * 0.03), blur: side * 0.06, color: NSColor.black.withAlphaComponent(0.45).cgColor)
    context.draw(donut, in: CGRect(x: (Double(context.width) - side) / 2, y: (Double(context.height) - side) / 2, width: side, height: side))
}

let layerJSON = """
{ "layers" : [ { "filename" : "Front.imagestacklayer" }, { "filename" : "Back.imagestacklayer" } ], "info" : { "version" : 1, "author" : "xcode" } }
"""
let layerContents = """
{ "info" : { "version" : 1, "author" : "xcode" } }
"""
for (stack, sizes) in [("App Icon.imagestack", [(400, 240, 1), (800, 480, 2)]), ("App Icon - App Store.imagestack", [(1280, 768, 1)])] {
    let stackURL = brand.appendingPathComponent(stack)
    write(layerJSON, stackURL)
    for (layer, opaque, draw) in [("Front", false, front), ("Back", true, sky)] {
        let layerURL = stackURL.appendingPathComponent("\(layer).imagestacklayer")
        write(layerContents, layerURL)
        let imageset = layerURL.appendingPathComponent("Content.imageset")
        var images: [String] = []
        for (w, h, scale) in sizes {
            let name = "\(layer)_\(w)x\(h).png"
            render(w, h, opaque: opaque, to: imageset.appendingPathComponent(name).creatingFolder(), draw)
            images.append("{ \"idiom\" : \"tv\", \"filename\" : \"\(name)\", \"scale\" : \"\(scale)x\" }")
        }
        write("{ \"images\" : [ \(images.joined(separator: ", ")) ], \"info\" : { \"version\" : 1, \"author\" : \"xcode\" } }", imageset)
    }
}

// Top Shelf: the logo on its sky (2.67:1 and 3.22:1)
for (set, width, crop) in [("Top Shelf Image.imageset", 1920, titleCrop(216, 360, 3200, 1200)),
                           ("Top Shelf Image Wide.imageset", 2320, titleCrop(27, 390, 3576, 1110))] {
    let url = brand.appendingPathComponent(set)
    var images: [String] = []
    for scale in [1, 2] {
        let name = "topshelf_\(width * scale)x\(720 * scale).png"
        render(width * scale, 720 * scale, opaque: true, to: url.appendingPathComponent(name).creatingFolder(), crop)
        images.append("{ \"idiom\" : \"tv\", \"filename\" : \"\(name)\", \"scale\" : \"\(scale)x\" }")
    }
    write("{ \"images\" : [ \(images.joined(separator: ", ")) ], \"info\" : { \"version\" : 1, \"author\" : \"xcode\" } }", url)
}

write("""
{ "assets" : [
  { "size" : "1280x768", "idiom" : "tv", "filename" : "App Icon - App Store.imagestack", "role" : "primary-app-icon" },
  { "size" : "400x240", "idiom" : "tv", "filename" : "App Icon.imagestack", "role" : "primary-app-icon" },
  { "size" : "2320x720", "idiom" : "tv", "filename" : "Top Shelf Image Wide.imageset", "role" : "top-shelf-image-wide" },
  { "size" : "1920x720", "idiom" : "tv", "filename" : "Top Shelf Image.imageset", "role" : "top-shelf-image" }
], "info" : { "version" : 1, "author" : "xcode" } }
""", brand)
write("{ \"info\" : { \"version\" : 1, \"author\" : \"xcode\" } }", assets)
print("Images pretes : \(brand.path)")

extension URL {
    // The URL itself, once its folder exists
    func creatingFolder() -> URL {
        try! FileManager.default.createDirectory(at: deletingLastPathComponent(), withIntermediateDirectories: true)
        return self
    }
}
