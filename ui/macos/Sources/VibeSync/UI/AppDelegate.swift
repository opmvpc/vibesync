// AppDelegate.swift — fenêtre unique AppKit hébergeant l'interface SwiftUI.
//
// Cycle de vie AppKit explicite (pas de @main ni de SwiftUI App) : c'est le
// chemin le plus stable et le plus ancien, et il rend la cible exécutable
// testable par SwiftPM.

import AppKit
import SwiftUI

public final class AppDelegate: NSObject, NSApplicationDelegate {

    // Magasin de réglages (suite alternative si VIBESYNC_SUITE est posée) et
    // pilote du harnais de test réel (nil hors mode auto) : les deux se lisent
    // dans l'environnement, avant toute construction de l'interface.
    let model = AppModel(store: Preferences.store(), auto: AutoPilot.fromEnvironment())
    private var window: NSWindow?

    public func applicationDidFinishLaunching(_ notification: Notification) {
        installMenu()

        let root = RootView().environmentObject(model)
        let win = NSWindow(
            contentRect: NSRect(x: 0, y: 0, width: 900, height: 620),
            styleMask: [.titled, .closable, .miniaturizable, .resizable],
            backing: .buffered,
            defer: false)
        win.title = "vibesync"
        win.contentView = NSHostingView(rootView: root)
        win.setFrameAutosaveName("vibesync.main")
        win.center()
        win.makeKeyAndOrderFront(nil)
        window = win
        // Une instance pilotée ne vole pas le premier plan : le harnais en
        // lance deux, elles se battraient pour l'activation.
        if !model.isAutoPiloted {
            NSApp.activate(ignoringOtherApps: true)
        }
        model.startAutoPilot()
    }

    public func applicationShouldTerminateAfterLastWindowClosed(_ sender: NSApplication) -> Bool {
        return true
    }

    public func applicationWillTerminate(_ notification: Notification) {
        model.shutdown()
    }

    /// Menu minimal : sans lui, ⌘Q et le copier/coller ne fonctionneraient pas
    /// (aucun nib n'est chargé).
    private func installMenu() {
        let mainMenu = NSMenu()

        let appItem = NSMenuItem()
        mainMenu.addItem(appItem)
        let appMenu = NSMenu()
        appMenu.addItem(withTitle: "À propos de vibesync",
                        action: #selector(NSApplication.orderFrontStandardAboutPanel(_:)),
                        keyEquivalent: "")
        appMenu.addItem(NSMenuItem.separator())
        appMenu.addItem(withTitle: "Masquer vibesync",
                        action: #selector(NSApplication.hide(_:)),
                        keyEquivalent: "h")
        appMenu.addItem(NSMenuItem.separator())
        appMenu.addItem(withTitle: "Quitter vibesync",
                        action: #selector(NSApplication.terminate(_:)),
                        keyEquivalent: "q")
        appItem.submenu = appMenu

        let editItem = NSMenuItem()
        mainMenu.addItem(editItem)
        let editMenu = NSMenu(title: "Édition")
        editMenu.addItem(withTitle: "Annuler", action: Selector(("undo:")), keyEquivalent: "z")
        editMenu.addItem(withTitle: "Rétablir", action: Selector(("redo:")), keyEquivalent: "Z")
        editMenu.addItem(NSMenuItem.separator())
        editMenu.addItem(withTitle: "Couper", action: Selector(("cut:")), keyEquivalent: "x")
        editMenu.addItem(withTitle: "Copier", action: Selector(("copy:")), keyEquivalent: "c")
        editMenu.addItem(withTitle: "Coller", action: Selector(("paste:")), keyEquivalent: "v")
        editMenu.addItem(withTitle: "Tout sélectionner", action: Selector(("selectAll:")), keyEquivalent: "a")
        editItem.submenu = editMenu

        NSApp.mainMenu = mainMenu
    }
}
