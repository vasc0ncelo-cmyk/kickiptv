// Testa o carregamento da lista em segundo plano (listload.hpp): copia salva, rede, queda no meio, cancelamento.
#include <assert.h>
#include <stdio.h>
#include <chrono>
#include <thread>
#include "../src/listload.hpp"

static std::string makeList(int n) {
    std::string s = "#EXTM3U\n";
    for (int i = 0; i < n; i++) s += "#EXTINF:-1 group-title=\"Canais\",Canal " + std::to_string(i) + "\nhttp://h/" + std::to_string(i) + "\n";
    return s;
}
// entrega o texto em pedacos de 'chunk' bytes (cortando linhas no meio, como a rede faz)
static ListLoader::Source feeder(const std::string &text, size_t chunk, bool result, int sleepMs = 0) {
    return [=](const std::string &, const ListLoader::Sink &sink, const std::atomic<bool> &) {
        for (size_t i = 0; i < text.size(); i += chunk) {
            if (sleepMs) std::this_thread::sleep_for(std::chrono::milliseconds(sleepMs));
            if (!sink(text.data() + i, std::min(chunk, text.size() - i))) return false;
        }
        return result;
    };
}
static bool waitDone(ListLoader &l, std::shared_ptr<Playlist> &pl, ListLoader::State &st, int ms = 5000) {
    for (int i = 0; i < ms / 5; i++) { if (l.take(pl, st)) return true; std::this_thread::sleep_for(std::chrono::milliseconds(5)); }
    return false;
}

int main() {
    std::shared_ptr<Playlist> pl; ListLoader::State st;
    // 1) copia salva: usa ela e NAO toca na rede
    { ListLoader l; bool netCalled = false;
      l.start("u", feeder(makeList(500), 777, true), [&](const std::string &, const ListLoader::Sink &, const std::atomic<bool> &) { netCalled = true; return false; });
      assert(waitDone(l, pl, st)); assert(st == ListLoader::Done && pl && pl->channels.size() == 500 && !netCalled && !l.fromNet()); }
    // 2) sem copia salva (era o bug: a lista ficava vazia): baixa da rede
    { ListLoader l;
      l.start("u", [](const std::string &, const ListLoader::Sink &, const std::atomic<bool> &) { return false; }, feeder(makeList(300), 1000, true));
      assert(waitDone(l, pl, st)); assert(st == ListLoader::Done && pl && pl->channels.size() == 300 && l.fromNet()); }
    // 3) copia salva vazia/ruim: cai na rede
    { ListLoader l;
      l.start("u", feeder("lixo sem lista\n", 4, true), feeder(makeList(40), 100, true));
      assert(waitDone(l, pl, st)); assert(st == ListLoader::Done && pl->channels.size() == 40); }
    // 4) rede cai no meio: aproveita o que chegou
    { ListLoader l; std::string t = makeList(1000); t.resize(t.size() / 2);
      l.start("u", nullptr, feeder(t, 500, false));
      assert(waitDone(l, pl, st)); assert(st == ListLoader::Partial && pl && pl->channels.size() > 100 && pl->channels.size() < 1000); }
    // 5) tudo falha
    { ListLoader l;
      l.start("u", nullptr, [](const std::string &, const ListLoader::Sink &, const std::atomic<bool> &) { return false; });
      assert(waitDone(l, pl, st)); assert(st == ListLoader::Failed && !pl); }
    // 6) cancelar no meio de um download lento: volta rapido, sem resultado
    { ListLoader l;
      l.start("u", nullptr, feeder(makeList(100000), 2000, true, 5));
      std::this_thread::sleep_for(std::chrono::milliseconds(60));
      assert(l.running() && l.items() > 0);
      auto t0 = std::chrono::steady_clock::now();
      assert(l.cancel(2.0));
      assert(std::chrono::steady_clock::now() - t0 < std::chrono::milliseconds(500));
      assert(waitDone(l, pl, st)); assert(st == ListLoader::Cancelled && !pl); }
    // 7) preso na rede (nunca devolve): cancel() nao trava o app, a thread e solta
    { ListLoader l; std::atomic<bool> release{false};
      l.start("u", nullptr, [&](const std::string &, const ListLoader::Sink &, const std::atomic<bool> &) { while (!release) std::this_thread::sleep_for(std::chrono::milliseconds(5)); return false; });
      std::this_thread::sleep_for(std::chrono::milliseconds(30));
      auto t0 = std::chrono::steady_clock::now();
      assert(!l.cancel(0.2) && l.stillAlive());
      assert(std::chrono::steady_clock::now() - t0 < std::chrono::milliseconds(600));
      release = true; std::this_thread::sleep_for(std::chrono::milliseconds(60)); }
    // 8) recomecar com outra lista depois de terminar
    { ListLoader l;
      l.start("a", nullptr, feeder(makeList(10), 50, true)); assert(waitDone(l, pl, st)); assert(pl->channels.size() == 10);
      l.start("b", nullptr, feeder(makeList(20), 50, true)); assert(waitDone(l, pl, st)); assert(pl->channels.size() == 20); }
    printf("OK\n");
    return 0;
}
