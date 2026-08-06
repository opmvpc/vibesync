// swift-tools-version:5.7
//
// vibesync — paquet SwiftPM du client macOS handmade (ADR-008) : aucune
// dépendance de package, uniquement les frameworks du système (Foundation,
// AppKit, SwiftUI, Security) et la couche C commune du dépôt.
//
// POURQUOI CE FICHIER EST À LA RACINE DU DÉPÔT (VS-031, phase 2 d'ADR-010)
// ------------------------------------------------------------------------
// SwiftPM refuse qu'une cible référence des sources situées hors de la racine
// du paquet, et un lien symbolique vers core/ serait fragile sur un checkout
// Windows (l'autre moitié d'ADR-010). La couche C commune vivant dans core/ et
// le client macOS dans ui/macos/, la seule racine qui contienne les deux est
// celle du dépôt : Package.swift y est donc monté, et chaque cible pointe son
// répertoire explicitement. Aucune source n'est dupliquée, aucun lien
// symbolique n'est créé, et build.bat compile exactement les mêmes fichiers.
//
// Trois cibles :
//   - VSCore        : la couche C commune (core/src portable + core/posix)
//   - VibeSync      : l'exécutable (réseau + VLC + interface) — son moteur de
//                     synchronisation EST VSCore, via le wrapper mince
//                     ui/macos/Sources/VibeSync/Engine/CoreEngine.swift
//   - VibeSyncTests : rejeu des 13 vecteurs de conformité — DEUX FOIS, par le
//                     wrapper (le chemin réel de l'application) et par l'API C
//                     brute — plus les tests unitaires
//
// Depuis VS-032 (phase 3 d'ADR-010) il n'y a plus de moteur Swift natif : la
// machine à états n'existe qu'en un exemplaire, commun aux deux clients.
import PackageDescription

let package = Package(
    name: "VibeSync",
    platforms: [
        .macOS(.v13)
    ],
    targets: [
        .target(
            name: "VSCore",
            path: "core",
            exclude: ["tests"],
            sources: ["src", "posix"],
            publicHeadersPath: "include",
            cSettings: [
                // Le répertoire des en-têtes publics sert aussi aux sources de
                // la cible elle-même (« #include "base.h" »).
                .headerSearchPath("include"),
                // Mêmes drapeaux que build.bat : ce qui échoue ici échouera
                // sous Windows, et réciproquement.
                .unsafeFlags([
                    "-ffp-contract=off",
                    "-Wall", "-Wextra", "-Werror",
                    "-Wshadow", "-Wvla",
                    "-Wstrict-prototypes", "-Wmissing-prototypes",
                ]),
            ]
        ),
        .executableTarget(
            name: "VibeSync",
            dependencies: ["VSCore"],
            path: "ui/macos/Sources/VibeSync"
        ),
        .testTarget(
            name: "VibeSyncTests",
            dependencies: ["VibeSync", "VSCore"],
            path: "ui/macos/Tests/VibeSyncTests"
        ),
    ],
    cLanguageStandard: .c11
)
