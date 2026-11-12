// Regras da atualizacao silenciosa da lista (listrefresh.hpp).
#include "../src/listrefresh.hpp"
#include <cassert>
#include <iostream>
using namespace listrefresh;
int main() {
    const long now = 1800000000L;                                   // um relogio valido
    // quando conferir a rede
    assert(due(now, 0));                                            // sem registro: confere
    assert(!due(now, now - 3600));                                  // gravada ha 1 h: nao
    assert(!due(now, now - kMinAgeSecs + 1));                       // quase 6 h: ainda nao
    assert(due(now, now - kMinAgeSecs));                            // 6 h: sim
    assert(due(now, now + 500));                                    // relogio voltou atras: sim
    assert(!due(1000, now - 99999) && due(1000, 0));                // sem hora confiavel: so se nunca registrou
    // resumo do conteudo
    { Fnv a, b, c; a.feed("#EXTM3U\n", 8); b.feed("#EXTM3U", 7); b.feed("\n", 1); c.feed("#EXTM3U\n#", 9);
      assert(a.hex() == b.hex());                                   // mesmo conteudo em pedacos diferentes = mesmo resumo
      assert(a.hex() != c.hex() && a.hex().size() == 16); }
    // cabecalho de lista
    assert(looksLikeM3U("#EXTM3U\n#EXTINF"));
    assert(looksLikeM3U("\xEF\xBB\xBF#EXTM3U"));                    // com BOM
    assert(looksLikeM3U("  \r\n#EXTM3U"));                          // espacos antes
    assert(!looksLikeM3U("<html>erro 403</html>") && !looksLikeM3U("{\"user_info\":{}}") && !looksLikeM3U("") && !looksLikeM3U("#EXTM3"));
    // aceitar a copia nova
    assert(accept(true, 5000000, 5200000));                         // um pouco menor: normal
    assert(accept(true, 5000000, 0));                               // sem copia antiga conhecida
    assert(!accept(false, 5000000, 5000000));                       // nao parece lista
    assert(!accept(true, 100, 0));                                  // minuscula
    assert(!accept(true, 2000000, 5000000));                        // encolheu mais da metade
    assert(accept(true, 2500000, 5000000));                         // exatamente a metade: ainda aceita
    // informacao guardada
    { std::string p = packInfo("00ff00ff00ff00ff", 123456, "http://x.com/get.php?a=1|2"), s; uint64_t by = 0;
      assert(unpackInfo(p, "http://x.com/get.php?a=1|2", s, by) && s == "00ff00ff00ff00ff" && by == 123456);   // a url pode ter '|'
      assert(!unpackInfo(p, "http://outra.com/lista.m3u", s, by) && s.empty() && by == 0);                      // outra lista: ignora
      assert(!unpackInfo("lixo", "u", s, by) && !unpackInfo("a|b", "u", s, by) && !unpackInfo("", "u", s, by)); }
    std::cout << "OK\n";
}
