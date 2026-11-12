#pragma once
// Lista padrao do app: e a UNICA lista (o menu nao tem mais "adicionar lista"; so "atualizar").
//
// Onde fica o endereco:
//  1) Se o build gerar src/defaultlist_secret.hpp (o workflow faz isso a partir do segredo LIST_URL do GitHub), vale esse arquivo.
//     Assim o usuario/senha NAO ficam no codigo publico do repositorio.
//  2) Senao vale o endereco abaixo (troque aqui se nao usar o segredo).
#if defined(__has_include)
#if __has_include("defaultlist_secret.hpp")
#include "defaultlist_secret.hpp"      // define: static const char *kSecretListUrl = "...";
#define KICKIPTV_HAS_SECRET_LIST 1
#endif
#endif

inline const char *defaultListUrl() {
#ifdef KICKIPTV_HAS_SECRET_LIST
    return kSecretListUrl;
#else
    return "http://180381.sbs/get.php?username=Dirce0273&password=Sar5438&type=m3u_plus&output=ts";
#endif
}
