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
    }
}
