import AppKit

// usage: swift title.swift <out.png> <fontName or .ttf path> <bgHex|none> <pointSize> <w> <h> <scale>
// Draws "Griddy": heavy black outline + hard drop shadow, white fill (NFL Street style).
let a = CommandLine.arguments
let out = a[1], fontArg = a[2], bg = a[3]
let size = CGFloat(Double(a[4])!), w = Int(a[5])!, h = Int(a[6])!, scale = Int(a[7])!

var fontName = fontArg
if fontArg.hasSuffix(".ttf") {
    let url = URL(fileURLWithPath: fontArg) as CFURL
    CTFontManagerRegisterFontsForURL(url, .process, nil)
    let provider = CGDataProvider(url: url)!
    fontName = CGFont(provider)!.postScriptName! as String
}
let font = NSFont(name: fontName, size: size)!
let text = "Griddy"

let rep = NSBitmapImageRep(bitmapDataPlanes: nil, pixelsWide: w * scale, pixelsHigh: h * scale,
    bitsPerSample: 8, samplesPerPixel: 4, hasAlpha: true, isPlanar: false,
    colorSpaceName: .deviceRGB, bytesPerRow: 0, bitsPerPixel: 0)!
rep.size = NSSize(width: w, height: h)
NSGraphicsContext.saveGraphicsState()
NSGraphicsContext.current = NSGraphicsContext(bitmapImageRep: rep)
if bg != "none" {
    let v = Int(bg, radix: 16)!
    NSColor(red: CGFloat((v >> 16) & 255) / 255, green: CGFloat((v >> 8) & 255) / 255,
            blue: CGFloat(v & 255) / 255, alpha: 1).setFill()
    NSRect(x: 0, y: 0, width: w, height: h).fill()
}
let cg = NSGraphicsContext.current!.cgContext
cg.setLineJoin(.round)

// center the word in the canvas, leaving room for outline and shadow
let ts = NSAttributedString(string: text, attributes: [.font: font]).size()
let p = NSPoint(x: (CGFloat(w) - ts.width) / 2 - 1, y: (CGFloat(h) - ts.height) / 2 + 6)

let shadow = NSShadow()
shadow.shadowOffset = NSSize(width: 3, height: -3)
shadow.shadowBlurRadius = 0
shadow.shadowColor = NSColor.black
let back: [NSAttributedString.Key: Any] = [.font: font, .strokeWidth: 14,
    .strokeColor: NSColor.black, .shadow: shadow]
NSAttributedString(string: text, attributes: back).draw(at: p)
let front: [NSAttributedString.Key: Any] = [.font: font, .foregroundColor: NSColor.white]
NSAttributedString(string: text, attributes: front).draw(at: p)
NSGraphicsContext.restoreGraphicsState()
try! rep.representation(using: .png, properties: [:])!.write(to: URL(fileURLWithPath: out))
