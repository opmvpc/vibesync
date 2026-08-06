// RootView.swift — bascule entre l'écran de connexion et l'écran de salle.

import SwiftUI

struct RootView: View {
    @EnvironmentObject var model: AppModel

    var body: some View {
        Group {
            if model.screen == AppModel.Screen.connect {
                ConnectView()
            } else {
                RoomView()
            }
        }
        .frame(minWidth: 760, minHeight: 560)
        // La feuille est ici : les Réglages s'ouvrent depuis les deux écrans
        // (bouton de l'écran de connexion, bandeau « introuvable » en salle).
        .sheet(isPresented: $model.showSettings) {
            SettingsView()
                .environmentObject(model)
        }
    }
}
