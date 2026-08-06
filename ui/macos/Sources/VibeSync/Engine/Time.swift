// Time.swift — arithmétique du temps du moteur.
//
// Le moteur raisonne en nanosecondes (Int64) depuis l'epoch Unix, exactement
// comme le port C (base.h) et comme la référence Go (time.Time). Les instants
// « pas encore armés » sont des Optional plutôt qu'une sentinelle : en Swift
// une soustraction sur Int64.min déclencherait un piège d'exécution.

import Foundation

/// Instant ou durée en nanosecondes.
public typealias Nanos = Int64

public enum VSTime {

    /// Horloge murale, résolution milliseconde (toutes les constantes du
    /// protocole sont en millisecondes ; on évite ainsi toute API Darwin).
    public static func now() -> Nanos {
        let ms = (Date().timeIntervalSince1970 * 1000.0).rounded()
        return Int64(ms) * 1_000_000
    }

    /// Conversion en millisecondes epoch (division plancher, comme le port C).
    public static func toUnixMs(_ ns: Nanos) -> Int64 {
        var ms = ns / 1_000_000
        if ns < 0 && ns % 1_000_000 != 0 {
            ms -= 1
        }
        return ms
    }

    public static func fromUnixMs(_ ms: Int64) -> Nanos {
        return ms * 1_000_000
    }

    /// Durée en secondes, avec la même arithmétique que time.Duration.Seconds()
    /// en Go (partie entière + reste) — indispensable pour reproduire les
    /// vecteurs au bit près.
    public static func seconds(_ dur: Nanos) -> Double {
        let sec = dur / 1_000_000_000
        let nsec = dur % 1_000_000_000
        return Double(sec) + Double(nsec) / 1e9
    }

    public static func milliseconds(_ ms: Int64) -> Nanos {
        return ms * 1_000_000
    }
}

/// Constantes de synchronisation — docs/protocol.md §Comportements client.
/// Toute valeur ici est gelée par test/vectors/*.json.
public enum Sync {
    public static let pollInterval: Nanos = 200 * 1_000_000
    public static let deadZoneSec: Double = 0.1
    public static let hardSeekSec: Double = 2.0
    public static let nudgeFast: Double = 1.05
    public static let nudgeSlow: Double = 0.95
    public static let nudgeExitSec: Double = 0.03
    public static let userSeekSec: Double = 3.0
    /// Départ/reprise de lecture : on cale VLC sur la position de référence
    /// avant de lancer la lecture plutôt que de compter sur le nudge (5 %/s).
    public static let startSeekSec: Double = 0.3
    public static let graceWindow: Nanos = 500 * 1_000_000
    public static let userHold: Nanos = 2000 * 1_000_000
    public static let pausedSeekSec: Double = 0.6
    public static let minRate: Double = 0.25
    public static let maxRate: Double = 4.0
    public static let pingEvery: Nanos = 2000 * 1_000_000
    public static let reportEvery: Nanos = 1000 * 1_000_000
    public static let offsetSamples: Int = 5
    public static let backoffMin: Nanos = 1000 * 1_000_000
    public static let backoffMax: Nanos = 10000 * 1_000_000

    /// Détecteur de buffering (l'interface HTTP de VLC n'expose pas d'état).
    public static let bufferWindow: Nanos = 700 * 1_000_000
    public static let bufferMinRatio: Double = 0.25

    /// Doublement borné 1 s → 10 s.
    public static func nextBackoff(_ current: Nanos) -> Nanos {
        let next = current <= 0 ? backoffMin : current * 2
        return next > backoffMax ? backoffMax : next
    }
}
