// The iOS app icon (assets/icon-ios.png): assets/icon.icns's crystal, full bleed (iOS rounds the corners itself).
//   xcrun swift tools/make_ios_icon.swift assets/icon-ios.png
import CoreGraphics
import Foundation
import ImageIO
import UniformTypeIdentifiers

let n = 1024
let cs = CGColorSpaceCreateDeviceRGB()
let ctx = CGContext(data: nil, width: n, height: n, bitsPerComponent: 8, bytesPerRow: 0, space: cs,
                    bitmapInfo: CGImageAlphaInfo.noneSkipLast.rawValue)!
func rgb(_ r: Int, _ g: Int, _ b: Int, _ a: CGFloat = 1) -> CGColor {
    CGColor(colorSpace: cs, components: [CGFloat(r) / 255, CGFloat(g) / 255, CGFloat(b) / 255, a])!
}
// the navy gradient, darker at the top (CoreGraphics' y is up)
let grad = CGGradient(colorsSpace: cs, colors: [rgb(0x2A, 0x40, 0x7A), rgb(0x17, 0x22, 0x46)] as CFArray, locations: [0, 1])!
ctx.drawLinearGradient(grad, start: CGPoint(x: 0, y: 0), end: CGPoint(x: 0, y: n), options: [])
// the crystal: four facets, lit from the upper left; the Mac icon's, scaled from its 824-pixel square
let s = CGFloat(n) / 824
func p(_ x: CGFloat, _ y: CGFloat) -> CGPoint { CGPoint(x: (x - 100) * s, y: CGFloat(n) - (y - 100) * s) }
let top = p(512, 185), left = p(344, 512), right = p(681, 512), bottom = p(512, 840), mid = p(512, 512)
for (a, b, c, color) in [(top, left, mid, rgb(0xC8, 0xEE, 0xFC)), (top, mid, right, rgb(0xAA, 0xBF, 0xC2)),
                         (left, bottom, mid, rgb(0xB0, 0xC8, 0xCE)), (mid, bottom, right, rgb(0x94, 0x99, 0x90))] {
    ctx.beginPath(); ctx.move(to: a); ctx.addLine(to: b); ctx.addLine(to: c); ctx.closePath()
    ctx.setFillColor(color); ctx.fillPath()
}
ctx.beginPath(); ctx.addLines(between: [top, right, bottom, left]); ctx.closePath()
ctx.setStrokeColor(rgb(255, 255, 255, 0.35)); ctx.setLineWidth(3 * s); ctx.setLineJoin(.round); ctx.strokePath()
let out = URL(fileURLWithPath: CommandLine.arguments[1])
let dest = CGImageDestinationCreateWithURL(out as CFURL, UTType.png.identifier as CFString, 1, nil)!
CGImageDestinationAddImage(dest, ctx.makeImage()!, nil)
CGImageDestinationFinalize(dest)
