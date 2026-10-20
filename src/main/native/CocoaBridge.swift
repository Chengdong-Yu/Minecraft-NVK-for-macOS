// Cocoa object ownership and presentation live in Swift. Vulkan/JNI retain C ABIs.
@preconcurrency import AppKit
import QuartzCore
import IOSurface
import ImageIO
import ObjectiveC.runtime

private final class NativeSurface {
    let container: CALayer
    let image = CALayer()
    let overlay: CATextLayer?
    let label: String
    var sampleStart = CACurrentMediaTime()
    var sampleFrames = 0
    var fps = 0.0
    var caption = ""

    init(_ container: CALayer) {
        self.container = container
        label = ProcessInfo.processInfo.environment["MC_FPS_LABEL"] ?? "Minecraft / NVK"
        overlay = ProcessInfo.processInfo.environment["MC_FPS_OVERLAY"] == "1" ? CATextLayer() : nil
        image.frame = container.bounds
        image.autoresizingMask = [.layerWidthSizable, .layerHeightSizable]
        image.contentsGravity = .resize
        image.isOpaque = true
        image.contentsScale = container.contentsScale
        CATransaction.begin()
        CATransaction.setDisableActions(true)
        container.addSublayer(image)
        if let text = overlay {
            text.font = "Menlo" as CFString
            text.fontSize = 15
            text.foregroundColor = CGColor(gray: 1, alpha: 1)
            text.backgroundColor = CGColor(gray: 0, alpha: 0.8)
            text.contentsScale = max(1, container.contentsScale)
            image.addSublayer(text)
        }
        CATransaction.commit()
    }

    func publish(_ surface: IOSurfaceRef?) {
        if surface != nil, overlay != nil {
            sampleFrames += 1
            let now = CACurrentMediaTime()
            let elapsed = now - sampleStart
            if elapsed >= 1 {
                fps = Double(sampleFrames) / elapsed
                sampleStart = now
                sampleFrames = 0
                caption = String(format: "%@ | %.1f FPS (present)\n%.0f x %.0f | Swift Cocoa + NVK", label, fps, container.bounds.width, container.bounds.height)
            }
        }
        CATransaction.begin()
        CATransaction.setDisableActions(true)
        image.contents = surface
        image.frame = container.bounds
        if let text = overlay {
            text.frame = CGRect(x: 12, y: max(0, image.bounds.height - 68), width: max(1, image.bounds.width - 24), height: 56)
            text.string = caption
        }
        CATransaction.commit()
        CATransaction.flush()
    }
}

@_cdecl("mc_cocoa_surface_create")
public func createSurface(_ container: UnsafeRawPointer?) -> UnsafeMutableRawPointer? {
    guard let container else { return nil }
    return autoreleasepool {
        let layer = Unmanaged<CALayer>.fromOpaque(container).takeUnretainedValue()
        return Unmanaged.passRetained(NativeSurface(layer)).toOpaque()
    }
}

@_cdecl("mc_cocoa_surface_destroy")
public func destroySurface(_ pointer: UnsafeMutableRawPointer?) {
    guard let pointer else { return }
    autoreleasepool {
        let surface = Unmanaged<NativeSurface>.fromOpaque(pointer).takeRetainedValue()
        surface.image.removeFromSuperlayer()
    }
}

@_cdecl("mc_cocoa_surface_publish")
public func publishSurface(_ pointer: UnsafeMutableRawPointer?, _ image: UnsafeMutableRawPointer?) {
    guard let pointer else { return }
    autoreleasepool {
        let surface = Unmanaged<NativeSurface>.fromOpaque(pointer).takeUnretainedValue()
        let contents = image.map { Unmanaged<IOSurfaceRef>.fromOpaque($0).takeUnretainedValue() }
        surface.publish(contents)
    }
}

@_cdecl("mc_cocoa_iosurface_create")
public func createIOSurface(_ width: UInt32, _ height: UInt32, _ pitch: Int, _ size: Int) -> UnsafeMutableRawPointer? {
    autoreleasepool {
        let description: [String: Any] = [kIOSurfaceWidth as String: width, kIOSurfaceHeight as String: height,
            kIOSurfaceBytesPerElement as String: 4, kIOSurfaceBytesPerRow as String: pitch,
            kIOSurfacePixelFormat as String: UInt32(0x42475241), kIOSurfaceAllocSize as String: size]
        guard let surface = IOSurfaceCreate(description as CFDictionary) else { return nil }
        return Unmanaged.passRetained(surface).toOpaque()
    }
}

@_cdecl("mc_cocoa_surface_size")
public func surfaceSize(_ pointer: UnsafeMutableRawPointer?, _ width: UnsafeMutablePointer<UInt32>?, _ height: UnsafeMutablePointer<UInt32>?) {
    guard let pointer else { width?.pointee = 1; height?.pointee = 1; return }
    let state = Unmanaged<NativeSurface>.fromOpaque(pointer).takeUnretainedValue()
    width?.pointee = UInt32(max(1, state.image.bounds.width * state.image.contentsScale))
    height?.pointee = UInt32(max(1, state.image.bounds.height * state.image.contentsScale))
}

@_cdecl("mc_cocoa_runloop")
public func runLoop(_ seconds: Double) {
    autoreleasepool { RunLoop.current.run(until: Date(timeIntervalSinceNow: seconds)) }
}

@_cdecl("mc_cocoa_window_layer")
public func windowLayer(_ pointer: UnsafeMutableRawPointer?) -> UnsafeMutableRawPointer? {
    guard let pointer else { return nil }
    return autoreleasepool {
        let window = Unmanaged<NSWindow>.fromOpaque(pointer).takeUnretainedValue()
        guard let view = window.contentView else { return nil }
        return viewLayer(Unmanaged.passUnretained(view).toOpaque())
    }
}

@_cdecl("mc_cocoa_view_layer")
public func viewLayer(_ pointer: UnsafeMutableRawPointer?) -> UnsafeMutableRawPointer? {
    guard let pointer else { return nil }
    return autoreleasepool {
        let view = Unmanaged<NSView>.fromOpaque(pointer).takeUnretainedValue()
        view.wantsLayer = true
        guard let layer = view.layer else { return nil }
        layer.contentsGravity = .resize
        layer.contentsScale = 1
        return Unmanaged.passRetained(layer).toOpaque()
    }
}

@_cdecl("mc_cocoa_view_size")
public func viewSize(_ pointer: UnsafeMutableRawPointer?, _ width: UnsafeMutablePointer<Int32>?, _ height: UnsafeMutablePointer<Int32>?) {
    var bounds = CGRect(x: 0, y: 0, width: 854, height: 480)
    if let pointer { bounds = Unmanaged<NSView>.fromOpaque(pointer).takeUnretainedValue().bounds }
    width?.pointee = Int32(max(1, bounds.width))
    height?.pointee = Int32(max(1, bounds.height))
}

@_cdecl("mc_cocoa_release")
public func releaseObject(_ pointer: UnsafeMutableRawPointer?) {
    if let pointer { Unmanaged<AnyObject>.fromOpaque(pointer).release() }
}

// Diagnostic captures use the same live presentation counter as the on-window overlay.
// Pixel readback and PNG creation only happen when MC_RENDER_CAPTURE_DIR is enabled.
@_cdecl("mc_cocoa_capture")
public func capture(_ pointer: UnsafeMutableRawPointer?, _ imagePointer: UnsafeMutableRawPointer?, _ width: UInt32, _ height: UInt32, _ pitch: Int, _ size: Int, _ path: UnsafePointer<CChar>?) -> Int32 {
    guard let imagePointer, let path else { return 0 }
    return autoreleasepool {
        let surface = Unmanaged<IOSurfaceRef>.fromOpaque(imagePointer).takeUnretainedValue()
        let base = IOSurfaceGetBaseAddress(surface)
        guard let provider = CGDataProvider(dataInfo: nil, data: base, size: size, releaseData: { _, _, _ in }),
              let image = CGImage(width: Int(width), height: Int(height), bitsPerComponent: 8, bitsPerPixel: 32,
                bytesPerRow: pitch, space: CGColorSpaceCreateDeviceRGB(),
                bitmapInfo: CGBitmapInfo(rawValue: CGBitmapInfo.byteOrder32Little.rawValue | CGImageAlphaInfo.premultipliedFirst.rawValue),
                provider: provider, decode: nil, shouldInterpolate: false, intent: .defaultIntent),
              let context = CGContext(data: nil, width: Int(width), height: Int(height), bitsPerComponent: 8,
                bytesPerRow: 0, space: CGColorSpaceCreateDeviceRGB(), bitmapInfo: CGImageAlphaInfo.premultipliedLast.rawValue) else { return 0 }
        context.draw(image, in: CGRect(x: 0, y: 0, width: Int(width), height: Int(height)))
        if let pointer {
            let state = Unmanaged<NativeSurface>.fromOpaque(pointer).takeUnretainedValue()
            if state.overlay != nil {
                let rect = CGRect(x: 12, y: max(0, Int(height) - 76), width: max(1, Int(width) - 24), height: 64)
                context.setFillColor(CGColor(gray: 0, alpha: 0.85)); context.fill(rect)
                NSGraphicsContext.saveGraphicsState()
                NSGraphicsContext.current = NSGraphicsContext(cgContext: context, flipped: false)
                (state.caption as NSString).draw(in: rect.insetBy(dx: 8, dy: 4), withAttributes: [
                    .font: NSFont.monospacedSystemFont(ofSize: 17, weight: .medium), .foregroundColor: NSColor.white])
                NSGraphicsContext.restoreGraphicsState()
            }
        }
        let url = URL(fileURLWithPath: String(cString: path))
        guard let output = context.makeImage(), let destination = CGImageDestinationCreateWithURL(url as CFURL, "public.png" as CFString, 1, nil) else { return 0 }
        CGImageDestinationAddImage(destination, output, nil)
        return CGImageDestinationFinalize(destination) ? 1 : 0
    }
}

private var auditInstalled = false
private let auditLock = NSLock()
public typealias AuditCallback = @convention(c) (UInt32) -> Void

@_cdecl("mc_cocoa_audit_install")
public func installAudit(_ callback: AuditCallback) -> Int32 {
    return autoreleasepool {
    auditLock.lock(); defer { auditLock.unlock() }
    if auditInstalled { return 1 }
    guard let cls = NSClassFromString("NVMTLCommandBuffer"),
          let commit = class_getInstanceMethod(cls, NSSelectorFromString("commit")) else { return 0 }
    let selector = NSSelectorFromString("commit")
    let original = unsafeBitCast(method_getImplementation(commit), to: (@convention(c) (AnyObject, Selector) -> Void).self)
    let block: @convention(block) (AnyObject) -> Void = { object in callback(0); original(object, selector) }
    class_replaceMethod(cls, selector, imp_implementationWithBlock(block), method_getTypeEncoding(commit))
    for (name, category, argument) in [("renderCommandEncoderWithDescriptor:", UInt32(1), 1),
        ("computeCommandEncoder", 2, 0), ("computeCommandEncoderWithDispatchType:", 2, 2), ("blitCommandEncoder", 3, 0)] {
        let sel = NSSelectorFromString(name)
        guard let method = class_getInstanceMethod(cls, sel) else { continue }
        let implementation = method_getImplementation(method)
        let replacement: IMP
        if argument == 2 {
            let call = unsafeBitCast(implementation, to: (@convention(c) (AnyObject, Selector, UInt) -> Unmanaged<AnyObject>?).self)
            let block: @convention(block) (AnyObject, UInt) -> AnyObject? = { object, value in callback(category); return call(object, sel, value)?.takeUnretainedValue() }
            replacement = imp_implementationWithBlock(block)
        } else if argument == 1 {
            let call = unsafeBitCast(implementation, to: (@convention(c) (AnyObject, Selector, AnyObject?) -> Unmanaged<AnyObject>?).self)
            let block: @convention(block) (AnyObject, AnyObject?) -> AnyObject? = { object, value in callback(category); return call(object, sel, value)?.takeUnretainedValue() }
            replacement = imp_implementationWithBlock(block)
        } else {
            let call = unsafeBitCast(implementation, to: (@convention(c) (AnyObject, Selector) -> Unmanaged<AnyObject>?).self)
            let block: @convention(block) (AnyObject) -> AnyObject? = { object in callback(category); return call(object, sel)?.takeUnretainedValue() }
            replacement = imp_implementationWithBlock(block)
        }
        class_replaceMethod(cls, sel, replacement, method_getTypeEncoding(method))
    }
    auditInstalled = true
    return 1
    }
}
