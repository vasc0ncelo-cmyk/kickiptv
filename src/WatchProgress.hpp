// WatchProgress.h - "Continuar Assistindo" para o kickiptv (PS4 / Orbis OS)
// Sem dependencias externas: JSON minimo embutido, escrita atomica, thread-safe.
#pragma once

#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>

namespace kick {

struct ProgressEntry {
    std::string id;           // ID do VOD / episodio
    int64_t positionSec = 0;  // currentTime em segundos
    int64_t durationSec = 0;  // duracao total (0 = desconhecida)
    int64_t updatedAt = 0;    // epoch (usado para limpar os mais antigos)
};

class WatchProgress {
public:
    // Ajustes
    static constexpr int     kSaveIntervalSec = 10;   // salvamento periodico
    static constexpr int64_t kMinResumeSec    = 15;   // abaixo disso nao vale salvar
    static constexpr int64_t kEndMarginSec    = 60;   // restando <= 60s = "terminou"
    static constexpr double  kEndFraction     = 0.95; // ou >= 95% assistido
    static constexpr size_t  kMaxEntries      = 500;  // limite de itens no arquivo

    static WatchProgress& Instance();

    // Chamar uma vez na inicializacao do app. Cria o diretorio se preciso
    // e le o JSON inteiro para a memoria (leituras seguintes sao instantaneas).
    // Ex.: Init("/data/kickiptv/saved_progress.json");
    void Init(const std::string& filePath);

    // Consulta (memoria, sem I/O). Retorna true se ha progresso valido p/ retomar.
    bool Get(const std::string& id, ProgressEntry& out) const;
    bool GetResumePoint(const std::string& id, int64_t& outSec) const;

    // Chamar a cada tick do player (ex.: 1x/s). Grava em disco no maximo a cada
    // kSaveIntervalSec. Use force=true ao pausar, parar, sair, fechar o player
    // ou suspender o app.
    void Update(const std::string& id, int64_t positionSec, int64_t durationSec,
                bool force = false);

    void Clear(const std::string& id);  // remove (ex.: "marcar como assistido")
    void Flush();                       // forca gravacao se houver mudancas

    static std::string FormatTime(int64_t sec);  // "1:23:45" ou "23:45"

private:
    WatchProgress() = default;
    WatchProgress(const WatchProgress&) = delete;
    WatchProgress& operator=(const WatchProgress&) = delete;

    bool IsNearEnd(int64_t pos, int64_t dur) const;
    void Prune();                                // exige mutex_ travado
    std::string Serialize() const;               // exige mutex_ travado
    bool WriteAtomic(const std::string& data);   // usa fileMutex_
    void Load();

    mutable std::mutex mutex_;   // protege entries_/dirty_/lastSave_
    std::mutex fileMutex_;       // serializa escrita em disco
    std::unordered_map<std::string, ProgressEntry> entries_;
    std::string path_;
    bool dirty_ = false;
    std::chrono::steady_clock::time_point lastSave_{};
};

}  // namespace kick
