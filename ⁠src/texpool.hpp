#pragma once
// Reuso da memoria de textura (quadros de video) do AvPlayer. Independente de plataforma (testavel no PC).
//
// PROBLEMA: cada vez que o app abre um AvPlayer novo, ele pede memoria "direta" para os quadros de video. O app NAO devolve essa
// memoria ao sistema ao fechar o player (devolver na hora causava erro no PS4), entao ela ficava presa para sempre. A live troca de
// "pedaco" abrindo um player novo a cada poucos segundos: depois de algum tempo a memoria direta acabava, nenhum player novo
// conseguia alocar os quadros e a imagem congelava (e a reconexao automatica falhava pelo mesmo motivo).
//
// SOLUCAO: o bloco que o player larga NAO volta ao sistema (continua do app, como antes), mas fica numa lista e e REUSADO pelo
// proximo player que pedir um bloco do mesmo tamanho/tipo. O consumo para de crescer.
#include <stddef.h>
#include <stdint.h>
#include <mutex>
#include <vector>

namespace texpool {

struct Block {
    void *addr = nullptr;
    long long phys = 0;      // endereco fisico (off_t no PS4)
    size_t size = 0;
    int type = 0;            // tipo da memoria (0 = Onion, 3 = Garlic)
};

class Pool {
public:
    // Procura um bloco guardado com exatamente 'size' bytes, do tipo 'type', cujo endereco respeite o alinhamento 'align'.
    bool take(size_t size, int type, size_t align, Block &out) {
        std::lock_guard<std::mutex> l(m_);
        for (size_t i = 0; i < v_.size(); i++) {
            const Block &b = v_[i];
            if (b.size != size || b.type != type) continue;
            if (align > 1 && ((uintptr_t)b.addr % align) != 0) continue;
            out = b; v_.erase(v_.begin() + (long)i); reused_++;
            return true;
        }
        return false;
    }
    void put(const Block &b) {
        if (!b.addr || b.size == 0) return;
        std::lock_guard<std::mutex> l(m_);
        for (const Block &x : v_) if (x.addr == b.addr) return;        // nunca guarda o mesmo bloco duas vezes
        v_.push_back(b);
    }
    size_t count() { std::lock_guard<std::mutex> l(m_); return v_.size(); }
    size_t bytes() { std::lock_guard<std::mutex> l(m_); size_t n = 0; for (auto &b : v_) n += b.size; return n; }
    size_t reused() { std::lock_guard<std::mutex> l(m_); return reused_; }
private:
    std::mutex m_;
    std::vector<Block> v_;
    size_t reused_ = 0;
};

}  // namespace texpool
