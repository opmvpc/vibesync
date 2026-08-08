// SettingsView.swift — panneau Réglages : chemin de VLC et dossiers médias
// (VS-026).
//
// Équivalent du panneau du client Windows (ui.c) : où trouver VLC, et la liste
// où l'on ira chercher le fichier qu'un participant a déclaré.

import SwiftUI

struct SettingsView: View {
    @EnvironmentObject var model: AppModel

    var body: some View {
        VStack(alignment: .leading, spacing: 14) {
            Text("Réglages")
                .font(.system(size: 18, weight: .semibold))

            Card("VLC") {
                VStack(alignment: .leading, spacing: 4) {
                    Text("Chemin de VLC (vide = détection automatique)")
                        .font(.system(size: 11))
                        .foregroundColor(Palette.secondary)
                    HStack(spacing: 8) {
                        TextField("Détection automatique", text: $model.vlcPath)
                            .textFieldStyle(.roundedBorder)
                            // À la frappe, comme sur Windows : l'état ci-dessous
                            // doit répondre tout de suite, sans validation.
                            .onChange(of: model.vlcPath) { _ in
                                model.vlcPathChanged()
                            }
                        Button("Parcourir…") {
                            model.browseVLC()
                        }
                    }
                    Text(model.vlcStatus.text)
                        .font(.system(size: 11))
                        .foregroundColor(SettingsView.color(model.vlcStatus.severity))
                        .lineLimit(2)
                        .truncationMode(.middle)
                        .fixedSize(horizontal: false, vertical: true)
                }
                Text("Choisissez VLC.app ou le binaire vlc. Ce réglage passe avant "
                     + "la variable d'environnement VIBESYNC_VLC.")
                    .font(.system(size: 11))
                    .foregroundColor(Palette.tertiary)
                    .fixedSize(horizontal: false, vertical: true)
            }

            Card("Dossiers médias (recherche du fichier d'un participant)") {
                if model.mediaDirs.isEmpty {
                    Text("Aucun dossier : ajoutez celui de vos films.")
                        .font(.system(size: 12))
                        .foregroundColor(Palette.warn)
                }
                ForEach(Array(model.mediaDirs.enumerated()), id: \.offset) { index, dir in
                    HStack(spacing: 8) {
                        Text(dir)
                            .font(.system(size: 11))
                            .foregroundColor(Palette.secondary)
                            .lineLimit(1)
                            .truncationMode(.middle)
                        Spacer()
                        Button("Retirer") {
                            model.removeMediaDir(at: index)
                        }
                        .controlSize(.small)
                    }
                }
                HStack {
                    Button("Ajouter un dossier…") {
                        model.addMediaDir()
                    }
                    .disabled(model.mediaDirs.count >= Preferences.maxMediaDirs)
                    Spacer()
                }
                Text("La recherche est récursive et bornée (profondeur ≤ \(MediaLibrary.maxDepth), "
                     + "≤ \(MediaLibrary.maxEntries) entrées). Le nom doit être identique, la casse "
                     + "n'a pas d'importance.")
                    .font(.system(size: 11))
                    .foregroundColor(Palette.tertiary)
                    .fixedSize(horizontal: false, vertical: true)
            }

            HStack(spacing: 10) {
                Text("client \(model.clientVersion)"
                     + (model.serverVersion.isEmpty ? "" : " · serveur \(model.serverVersion)"))
                    .font(.system(size: 11))
                    .foregroundColor(Palette.tertiary)
                Spacer()
                Button("Fermer") {
                    model.showSettings = false
                }
                .keyboardShortcut(.defaultAction)
            }
        }
        .padding(20)
        .frame(width: 520)
        // VLC a pu être installé (ou déplacé) depuis le lancement : l'état
        // affiché est recalculé à chaque ouverture du panneau.
        .onAppear {
            model.refreshVLCStatus()
        }
    }

    /// Couleur de la ligne d'état sous le champ VLC.
    private static func color(_ severity: VLCPathStatus.Severity) -> Color {
        switch severity {
        case .ok:
            return Palette.good
        case .info:
            return Palette.secondary
        case .warn:
            return Palette.warn
        case .error:
            return Palette.bad
        }
    }
}
