// Theme.swift — palette et petits composants partagés.
//
// Uniquement des couleurs sémantiques du système : le thème clair/sombre suit
// automatiquement les réglages de macOS, sans code de bascule. Les API
// utilisées ici sont volontairement anciennes (macOS 11/12) pour ne dépendre
// de rien de récent.

import SwiftUI

enum Palette {
    static let panel = Color(nsColor: .controlBackgroundColor)
    static let separator = Color(nsColor: .separatorColor)
    static let secondary = Color(nsColor: .secondaryLabelColor)
    static let tertiary = Color(nsColor: .tertiaryLabelColor)
    static let accent = Color.accentColor
    static let good = Color.green
    static let warn = Color.orange
    static let bad = Color.red

    static func level(_ name: String) -> Color {
        switch name {
        case "error":
            return bad
        case "warn":
            return warn
        default:
            return accent
        }
    }
}

/// Panneau arrondi sobre, utilisé pour toutes les sections.
struct Card<Content: View>: View {
    private let title: String?
    private let content: Content

    init(_ title: String? = nil, @ViewBuilder content: () -> Content) {
        self.title = title
        self.content = content()
    }

    var body: some View {
        VStack(alignment: .leading, spacing: 10) {
            if let title = title {
                Text(title.uppercased())
                    .font(.system(size: 10, weight: .semibold))
                    .foregroundColor(Palette.tertiary)
            }
            content
        }
        .padding(14)
        .frame(maxWidth: .infinity, alignment: .leading)
        .background(Palette.panel)
        .clipShape(RoundedRectangle(cornerRadius: 10, style: .continuous))
        .overlay(
            RoundedRectangle(cornerRadius: 10, style: .continuous)
                .stroke(Palette.separator, lineWidth: 1)
        )
    }
}

/// Petite pastille d'état (prêt, latence, drift…).
struct Badge: View {
    var text: String
    var color: Color

    var body: some View {
        Text(text)
            .font(.system(size: 11, weight: .medium).monospacedDigit())
            .padding(.horizontal, 7)
            .padding(.vertical, 2)
            .background(color.opacity(0.16))
            .foregroundColor(color)
            .clipShape(Capsule())
    }
}

/// Barre de position cliquable.
struct PositionBar: View {
    var position: Double
    var duration: Double
    var enabled: Bool
    var onSeek: (Double) -> Void

    private var fraction: Double {
        if duration <= 0 || !position.isFinite {
            return 0
        }
        let f = position / duration
        if f < 0 {
            return 0
        }
        return f > 1 ? 1 : f
    }

    var body: some View {
        GeometryReader { geo in
            ZStack(alignment: .leading) {
                Capsule()
                    .fill(Palette.separator)
                    .frame(height: 6)
                Capsule()
                    .fill(enabled ? Palette.accent : Palette.tertiary)
                    .frame(width: max(0, geo.size.width * fraction), height: 6)
            }
            .frame(width: geo.size.width, height: geo.size.height, alignment: .leading)
            .contentShape(Rectangle())
            .gesture(
                DragGesture(minimumDistance: 0)
                    .onEnded { value in
                        if !enabled || duration <= 0 || geo.size.width <= 0 {
                            return
                        }
                        var f = value.location.x / geo.size.width
                        if f < 0 {
                            f = 0
                        }
                        if f > 1 {
                            f = 1
                        }
                        onSeek(f * duration)
                    }
            )
        }
        .frame(height: 18)
    }
}
