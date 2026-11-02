import SwiftUI
import AppKit

public struct SipVideoViewRepresentable: NSViewRepresentable {
    public enum ViewType {
        case remote
        case localPreview
    }

    public let viewType: ViewType
    public let onAttach: (SipMacVideoView, ViewType) -> Void

    public init(viewType: ViewType, onAttach: @escaping (SipMacVideoView, ViewType) -> Void) {
        self.viewType = viewType
        self.onAttach = onAttach
    }

    public func makeNSView(context: Context) -> SipMacVideoView {
        let view = SipMacVideoView(frame: .zero)
        onAttach(view, viewType)
        return view
    }

    public func updateNSView(_ nsView: SipMacVideoView, context: Context) {
        // Layout updates handled automatically by CALayer contentsGravity
    }
}
