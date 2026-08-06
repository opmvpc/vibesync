// main.swift — point d'entrée.
//
// Démarrage AppKit explicite. Ce fichier est le seul à contenir du code de
// haut niveau ; tout le reste est du type nommé, ce qui permet à SwiftPM de
// lier la cible exécutable dans le bundle de tests.

import AppKit

let application = NSApplication.shared
let mainDelegate = AppDelegate()
application.delegate = mainDelegate
application.setActivationPolicy(.regular)
application.run()
