// swift-tools-version:5.7
//
// vibesync — client macOS handmade (ADR-008) : aucune dépendance de package,
// uniquement les frameworks du système (Foundation, AppKit, SwiftUI, Security).
//
// Deux cibles :
//   - VibeSync      : l'exécutable (moteur + réseau + VLC + interface)
//   - VibeSyncTests : le rejeu des 12 vecteurs de conformité + tests unitaires
//
// Si `swift test` échoue à lier la cible exécutable (limitation SwiftPM selon
// la version de la toolchain), voir docs/build-macos.md §« si erreurs de
// build » : la parade est d'extraire une cible bibliothèque, sans toucher au
// code source.
import PackageDescription

let package = Package(
    name: "VibeSync",
    platforms: [
        .macOS(.v13)
    ],
    targets: [
        .executableTarget(
            name: "VibeSync",
            path: "Sources/VibeSync"
        ),
        .testTarget(
            name: "VibeSyncTests",
            dependencies: ["VibeSync"],
            path: "Tests/VibeSyncTests"
        ),
    ]
)
