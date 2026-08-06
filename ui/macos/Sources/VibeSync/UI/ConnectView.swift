// ConnectView.swift — écran de connexion.

import SwiftUI

struct ConnectView: View {
    @EnvironmentObject var model: AppModel

    var body: some View {
        VStack(spacing: 0) {
            Spacer(minLength: 24)
            VStack(alignment: .leading, spacing: 18) {
                VStack(alignment: .leading, spacing: 4) {
                    Text("vibesync")
                        .font(.system(size: 30, weight: .semibold))
                    Text("Regardez vos fichiers, chacun chez soi, à la même seconde.")
                        .font(.system(size: 12))
                        .foregroundColor(Palette.secondary)
                }

                Card("Salon") {
                    field("Serveur", text: $model.serverURL, placeholder: "wss://vibesync.exemple.fr/ws")
                    field("Pseudo", text: $model.pseudo, placeholder: "thib")
                    field("Salle", text: $model.room, placeholder: "salon")
                    VStack(alignment: .leading, spacing: 4) {
                        Text("Mot de passe du serveur (facultatif)")
                            .font(.system(size: 11))
                            .foregroundColor(Palette.secondary)
                        SecureField("", text: $model.password)
                            .textFieldStyle(.roundedBorder)
                    }
                }

                if !model.formError.isEmpty {
                    Text(model.formError)
                        .font(.system(size: 12))
                        .foregroundColor(Palette.bad)
                }

                HStack {
                    Spacer()
                    Button("Rejoindre la salle") {
                        model.connect()
                    }
                    .keyboardShortcut(.defaultAction)
                    .buttonStyle(.borderedProminent)
                    .controlSize(.large)
                }
            }
            .frame(maxWidth: 460)
            .padding(.horizontal, 28)
            Spacer(minLength: 24)
        }
        .frame(maxWidth: .infinity, maxHeight: .infinity)
    }

    private func field(_ label: String, text: Binding<String>, placeholder: String) -> some View {
        VStack(alignment: .leading, spacing: 4) {
            Text(label)
                .font(.system(size: 11))
                .foregroundColor(Palette.secondary)
            TextField(placeholder, text: text)
                .textFieldStyle(.roundedBorder)
                .onSubmit {
                    model.connect()
                }
        }
    }
}
