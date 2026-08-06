// RoomView.swift — écran de salle : participants, lecture, chat, toasts.

import SwiftUI

struct RoomView: View {
    @EnvironmentObject var model: AppModel

    var body: some View {
        VStack(spacing: 0) {
            header
            Divider()
            notices
            HStack(alignment: .top, spacing: 14) {
                VStack(spacing: 14) {
                    playbackCard
                    participantsCard
                    Spacer(minLength: 0)
                }
                .frame(minWidth: 380)
                chatCard
                    .frame(width: 280)
            }
            .padding(14)
        }
        .overlay(alignment: .top) {
            toastStack
        }
    }

    // MARK: En-tête

    private var header: some View {
        HStack(spacing: 10) {
            Circle()
                .fill(model.connected ? Palette.good : Palette.warn)
                .frame(width: 8, height: 8)
            VStack(alignment: .leading, spacing: 1) {
                Text(model.room)
                    .font(.system(size: 13, weight: .semibold))
                Text(model.connectionLabel)
                    .font(.system(size: 11))
                    .foregroundColor(Palette.secondary)
            }
            Spacer()
            if model.latencyMs > 0 {
                Badge(text: "\(model.latencyMs) ms", color: Palette.secondary)
            }
            Button("Réglages…") {
                model.showSettings = true
            }
            Button("Quitter") {
                model.leave()
            }
        }
        .padding(.horizontal, 14)
        .padding(.vertical, 10)
    }

    // MARK: Bandeaux

    /// Invitations empilées, non bloquantes : mise à jour (VS-023), fichier
    /// d'un participant (VS-026), fichier introuvable.
    private var notices: some View {
        VStack(spacing: 0) {
            if model.showUpdateBanner {
                NoticeBar(text: model.updateBannerText, color: Palette.accent) {
                    model.openDownloadPage()
                } onClose: {
                    model.showUpdateBanner = false
                }
            }
            if model.showWatchBanner {
                NoticeBar(text: "\(model.watchWho) regarde \(model.watchFile) — cliquer pour l'ouvrir chez vous",
                          color: Palette.good,
                          busy: model.mediaSearching) {
                    model.openWatchedFile()
                } onClose: {
                    model.dismissWatchBanner()
                }
            }
            if model.showMediaNotice {
                NoticeBar(text: model.mediaNotice, color: Palette.warn) {
                    model.openSettingsFromNotice()
                } onClose: {
                    model.dismissMediaNotice()
                }
            }
        }
    }

    // MARK: Lecture

    private var playbackCard: some View {
        Card("Lecture") {
            HStack(spacing: 10) {
                Text(model.mediaLabel)
                    .font(.system(size: 12))
                    .lineLimit(1)
                    .truncationMode(.middle)
                Spacer()
                if model.buffering {
                    Badge(text: "mise en tampon", color: Palette.warn)
                }
                if !model.correctionLabel.isEmpty {
                    Badge(text: model.correctionLabel, color: Palette.accent)
                }
                Badge(text: model.driftLabel, color: driftColor)
            }

            PositionBar(position: model.positionSec,
                        duration: model.durationSec,
                        enabled: model.vlcRunning) { seconds in
                model.seek(to: seconds)
            }

            HStack(spacing: 10) {
                Text(AppModel.timeLabel(model.positionSec))
                    .font(.system(size: 11).monospacedDigit())
                    .foregroundColor(Palette.secondary)
                Spacer()
                Text(AppModel.timeLabel(model.durationSec))
                    .font(.system(size: 11).monospacedDigit())
                    .foregroundColor(Palette.secondary)
            }

            HStack(spacing: 10) {
                Button {
                    model.togglePlayback()
                } label: {
                    Label(model.roomPaused ? "Lecture" : "Pause",
                          systemImage: model.roomPaused ? "play.fill" : "pause.fill")
                        .frame(width: 90)
                }
                .buttonStyle(.borderedProminent)
                .disabled(!model.vlcRunning)

                Button(model.vlcRunning ? "Changer de fichier" : "Ouvrir un fichier…") {
                    model.chooseFile()
                }

                Spacer()

                Button {
                    model.toggleReady()
                } label: {
                    Label(model.ready ? "Prêt" : "Je suis prêt",
                          systemImage: model.ready ? "checkmark.circle.fill" : "circle")
                        .frame(width: 110)
                }
                .buttonStyle(.borderedProminent)
                .tint(model.ready ? Palette.good : Palette.accent)
                .controlSize(.large)
            }
        }
    }

    private var driftColor: Color {
        let d = abs(model.driftSec)
        if d < 0.1 {
            return Palette.good
        }
        return d < 2 ? Palette.warn : Palette.bad
    }

    // MARK: Participants

    private var participantsCard: some View {
        Card("Participants (\(model.users.count))") {
            if model.users.isEmpty {
                Text("Personne d'autre pour l'instant.")
                    .font(.system(size: 12))
                    .foregroundColor(Palette.secondary)
            }
            ForEach(model.users, id: \.id) { user in
                HStack(spacing: 8) {
                    Circle()
                        .fill(user.ready ? Palette.good : Palette.tertiary)
                        .frame(width: 7, height: 7)
                    Text(user.name)
                        .font(.system(size: 12, weight: .medium))
                    if user.hasFile && !user.fileName.isEmpty {
                        Text(user.fileName)
                            .font(.system(size: 11))
                            .foregroundColor(Palette.tertiary)
                            .lineLimit(1)
                            .truncationMode(.middle)
                    }
                    Spacer()
                    if user.latencyMs > 0 {
                        Badge(text: "\(user.latencyMs) ms", color: Palette.secondary)
                    }
                    Badge(text: user.ready ? "prêt" : "pas prêt",
                          color: user.ready ? Palette.good : Palette.tertiary)
                }
            }
        }
    }

    // MARK: Chat

    private var chatCard: some View {
        Card("Chat") {
            ScrollViewReader { proxy in
                ScrollView {
                    VStack(alignment: .leading, spacing: 6) {
                        ForEach(model.chatLines) { line in
                            VStack(alignment: .leading, spacing: 1) {
                                HStack(spacing: 6) {
                                    Text(line.from)
                                        .font(.system(size: 11, weight: .semibold))
                                    Text(line.time)
                                        .font(.system(size: 10))
                                        .foregroundColor(Palette.tertiary)
                                }
                                Text(line.text)
                                    .font(.system(size: 12))
                                    .textSelection(.enabled)
                            }
                            .frame(maxWidth: .infinity, alignment: .leading)
                            .id(line.id)
                        }
                    }
                    .padding(.trailing, 4)
                }
                .frame(minHeight: 220)
                .onChange(of: model.chatLines.count) { _ in
                    if let last = model.chatLines.last {
                        proxy.scrollTo(last.id, anchor: .bottom)
                    }
                }
            }
            HStack(spacing: 6) {
                TextField("Message…", text: $model.draft)
                    .textFieldStyle(.roundedBorder)
                    .onSubmit {
                        model.sendDraft()
                    }
                Button("Envoyer") {
                    model.sendDraft()
                }
            }
        }
    }

    // MARK: Toasts

    private var toastStack: some View {
        VStack(spacing: 6) {
            ForEach(model.toasts) { toast in
                Text(toast.text)
                    .font(.system(size: 12))
                    .padding(.horizontal, 12)
                    .padding(.vertical, 7)
                    .background(Palette.level(toast.level).opacity(0.18))
                    .foregroundColor(Palette.level(toast.level))
                    .clipShape(RoundedRectangle(cornerRadius: 8, style: .continuous))
            }
        }
        .padding(.top, 8)
        .allowsHitTesting(false)
    }
}
